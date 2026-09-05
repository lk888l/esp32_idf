#!/usr/bin/env python3
"""Hardware regression checks for the running M5-StickS3 BLE API.

Prerequisites: turn on host Bluetooth, install Bleak (python -m pip install bleak),
boot the final firmware, and approve any system pairing prompt. The firmware
requires encrypted Secure Connections pairing and stores bonds in NVS.

Pair once, reboot the device without erasing NVS, then run this script again to
verify stored-bond restoration. Every session retains the existing host/device
bonds. The checks send only ping, status, and echo; no API token is needed.
Transport/backend errors fail immediately with their original traceback.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from connectivity_client import RX_UUID, TX_UUID, decode_response, encode_request, positive_timeout


class CheckFailed(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailed(message)


def positive_sessions(value: str) -> int:
    count = int(value)
    if count < 1:
        raise argparse.ArgumentTypeError("sessions must be at least 1")
    return count


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    target = result.add_mutually_exclusive_group()
    target.add_argument("--ble", metavar="ADDRESS", help="device address (MAC or macOS UUID)")
    target.add_argument("--name", default="M5StickS3", metavar="PREFIX",
                        help="advertised name prefix (default: M5StickS3)")
    result.add_argument("--sessions", type=positive_sessions, default=2,
                        help="connect/test/disconnect sessions (default: 2)")
    result.add_argument("--scan-timeout", type=positive_timeout, default=10.0,
                        help="discovery seconds per session (default: 10)")
    result.add_argument("--connect-timeout", type=positive_timeout, default=25.0,
                        help="connection/pairing deadline in seconds (default: 25)")
    result.add_argument("--io-timeout", type=positive_timeout, default=10.0,
                        help="deadline for each GATT operation/disconnect (default: 10)")
    result.add_argument("--notification-timeout", type=positive_timeout, default=3.0,
                        help="ready notification deadline in seconds (default: 3)")
    return result


async def discover(scanner, args):
    # Discover afresh for every connection instead of retaining a backend's
    # device wrapper across sessions. This does not clear any bond or GATT cache.
    found = await asyncio.wait_for(
        scanner.discover(timeout=args.scan_timeout, return_adv=True),
        timeout=args.scan_timeout + args.io_timeout,
    )
    matches = []
    for device, advertisement in found.values():
        name = advertisement.local_name or device.name or ""
        selected = (device.address.casefold() == args.ble.casefold() if args.ble
                    else name.startswith(args.name))
        if selected:
            matches.append((device, name))
    require(bool(matches), "device not advertising; check Bluetooth, firmware, and target selection")
    require(len(matches) == 1, "multiple devices match; select one explicitly with --ble ADDRESS")
    return matches[0]


async def exchange(client, notices: asyncio.Queue, args, session: int,
                   request_id: int, operation: str, **fields):
    require(not notices.qsize(), f"session {session}: unexpected extra notification before {operation}")
    payload = encode_request(operation, request_id, **fields)
    await asyncio.wait_for(client.write_gatt_char(RX_UUID, payload, response=True), args.io_timeout)
    raw = bytes(await asyncio.wait_for(client.read_gatt_char(TX_UUID), args.io_timeout))
    require(0 < len(raw) <= 512, f"{operation}: invalid GATT response length")
    result = decode_response(raw)
    require(result.get("v") == 1, f"{operation}: incorrect API version")
    require(result.get("ok") is True, f"{operation}: device rejected request ({result.get('error')})")
    require(result.get("id") == request_id, f"{operation}: response ID mismatch")
    try:
        notice = await asyncio.wait_for(notices.get(), args.notification_timeout)
    except asyncio.TimeoutError as error:
        raise CheckFailed(f"session {session}: {operation} ready notification missing") from error
    ready = json.loads(notice)
    require(ready == {"ready": len(raw)}, f"{operation}: ready notification length mismatch")
    print(f"PASS session={session} {operation} response_bytes={len(raw)} ready={len(raw)}", flush=True)
    return result, raw


async def run_session(client_type, device, args, session: int) -> None:
    client = client_type(device, pair=True, timeout=args.connect_timeout)
    notices: asyncio.Queue[bytes] = asyncio.Queue()
    try:
        await asyncio.wait_for(client.connect(), args.connect_timeout)
        print(f"CONNECTED session={session} backend_mtu={client.mtu_size}", flush=True)
        await asyncio.wait_for(
            client.start_notify(TX_UUID, lambda _sender, data: notices.put_nowait(bytes(data))),
            args.io_timeout,
        )
        request_id = (session - 1) * 3 + 1
        ping, _ = await exchange(client, notices, args, session, request_id, "ping")
        require(ping.get("reply") == "pong", "ping: reply mismatch")
        # Security telemetry is published by the application's main task.
        # Allow that task to publish the link state refreshed by the first RX.
        await asyncio.sleep(0.2)
        status, raw = await exchange(client, notices, args, session, request_id + 1, "status")
        require(status.get("ble") == "connected", "status: BLE link is not connected")
        require(status.get("ble_secure") == 1, "status: BLE link is not encrypted")
        require(status.get("ble_bonded") == 1, "status: BLE link is not bonded")
        require("token" not in status and "password" not in status, "status exposed secret fields")
        # BlueZ may report client.mtu_size=23 after negotiation. Use the
        # firmware's measured MTU to prove this was a complete long read.
        mtu = status.get("mtu")
        require(type(mtu) is int and 23 <= mtu <= 517, "status: invalid negotiated MTU")
        require(len(raw) > mtu, "status: response did not exceed negotiated MTU; long read untested")
        print(f"PASS session={session} secure=1 bonded=1 long_read_bytes={len(raw)} mtu={mtu}", flush=True)
        echo_text = "BLE" + "x" * 125
        echo, _ = await exchange(client, notices, args, session, request_id + 2, "echo", data=echo_text)
        require(echo.get("data") == echo_text, "echo: 128-byte round trip mismatch")
        require(not notices.qsize(), f"session {session}: unexpected extra notifications")
    finally:
        # Disconnect directly so the subscribed CCCD can remain in the bond
        # record; the next session exercises its restoration. Do not unpair.
        await asyncio.wait_for(client.disconnect(), args.io_timeout)


async def run(args) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise RuntimeError("Install the test dependency: python -m pip install bleak") from error
    print("Host Bluetooth must already be on; approve system pairing prompts.", flush=True)
    for session in range(1, args.sessions + 1):
        device, name = await discover(BleakScanner, args)
        print(f"FOUND session={session} name={name} address={device.address}", flush=True)
        await run_session(BleakClient, device, args, session)
        if session != args.sessions:
            await asyncio.sleep(1.0)
    print(f"PASS BLE hardware regression: {args.sessions} sessions, encryption, bonds, long reads, echo, notifications",
          flush=True)


def main(argv: list[str] | None = None) -> int:
    arguments = parser()
    args = arguments.parse_args(argv)
    if not args.ble and not args.name:
        arguments.error("--name prefix must not be empty")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())