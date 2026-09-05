#!/usr/bin/env python3
"""M5-StickS3 API v1 client. HTTP uses only Python's standard library.

BLE additionally needs bleak and an encrypted paired connection:
    python -m pip install bleak
    python connectivity_client.py --ble AA:BB:CC:DD:EE:FF status

Set M5_API_TOKEN for WiFi changes. Set M5_WIFI_PASSWORD or enter it at the
hidden prompt. Neither secret is written to a file or printed by this tool.
"""

from __future__ import annotations

import argparse
import asyncio
import getpass
import json
import math
import os
import string
import sys
import urllib.error
import urllib.parse
import urllib.request

MAX_REQUEST_BYTES = 256
MAX_RESPONSE_BYTES = 768
RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


def positive_timeout(value: str) -> float:
    timeout = float(value)
    if not math.isfinite(timeout) or timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be a finite positive number")
    return timeout


def encode_request(operation: str, request_id: int = 1, **fields: object) -> bytes:
    message = {"v": 1, "id": request_id, "op": operation, **fields}
    payload = json.dumps(message, ensure_ascii=False, separators=(",", ":"),
                         allow_nan=False).encode("utf-8")
    if len(payload) > MAX_REQUEST_BYTES:
        raise ValueError(f"request exceeds the {MAX_REQUEST_BYTES} byte API limit")
    return payload


def decode_response(payload: bytes) -> dict:
    if not payload or len(payload) > MAX_RESPONSE_BYTES:
        raise ValueError("device response is empty or exceeds the API limit")
    try:
        result = json.loads(payload)
    except (ValueError, UnicodeError) as error:
        raise ValueError("device returned an invalid JSON response") from error
    if not isinstance(result, dict):
        raise ValueError("device response must be a JSON object")
    return result


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        # Keep credentials on the endpoint explicitly selected by the caller.
        return None


def http_exchange(url: str, payload: bytes | None, timeout: float) -> tuple[int, dict]:
    """Send one request; payload=None selects GET /api/v1/status.

    Raw bytes are accepted here so the regression script can test rejection of
    malformed/oversized traffic. Normal callers use encode_request first.
    """
    endpoint = urllib.parse.urlsplit(url)
    if (endpoint.scheme not in ("http", "https") or not endpoint.hostname or
            endpoint.username is not None or endpoint.password is not None or
            endpoint.query or endpoint.fragment):
        raise ValueError("--url must be an HTTP(S) base URL without credentials/query/fragment")
    path = "/api/v1/status" if payload is None else "/api/v1/command"
    request = urllib.request.Request(
        url.rstrip("/") + path, data=payload,
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="GET" if payload is None else "POST",
    )
    # Local device traffic should not be forwarded to system HTTP proxies.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        response = opener.open(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    except (urllib.error.URLError, OSError) as error:
        raise RuntimeError(f"HTTP connection failed ({type(error).__name__})") from error
    with response:
        return response.code, decode_response(response.read(MAX_RESPONSE_BYTES + 1))


async def ble_exchange(address: str, payload: bytes, timeout: float) -> dict:
    try:
        from bleak import BleakClient
    except ImportError as error:
        raise RuntimeError("BLE transport requires: python -m pip install bleak") from error

    async def transaction() -> dict:
        # RX and TX require encryption. Windows may display its pairing dialog;
        # the firmware uses Secure Connections Just Works with NVS-persisted bonds.
        async with BleakClient(address, timeout=timeout, pair=True) as client:
            try:
                await client.write_gatt_char(RX_UUID, payload, response=True)
            except Exception as error:
                # BlueZ reports MTU=23 even after negotiation, so do not reject
                # based solely on client.mtu_size. The server enforces MTU-3.
                raise RuntimeError(
                    f"BLE write failed ({type(error).__name__}); encrypted link and "
                    f"negotiated ATT MTU >= {len(payload) + 3} are required"
                ) from error
            # Write completion means the synchronous command callback has finished.
            # A full GATT read/read-long gets the result; ready notifications are hints.
            return decode_response(bytes(await client.read_gatt_char(TX_UUID)))

    try:
        return await asyncio.wait_for(transaction(), timeout=timeout)
    except (RuntimeError, ValueError):
        raise
    except Exception as error:
        raise RuntimeError(f"BLE transaction failed ({type(error).__name__})") from error


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument("--url", default="http://192.168.4.1", help="device HTTP base URL")
    result.add_argument("--ble", metavar="ADDRESS", help="use encrypted BLE instead of HTTP")
    result.add_argument("--timeout", type=positive_timeout, default=30.0, help="transport timeout in seconds (default: 30)")
    result.add_argument("--token", default=os.environ.get("M5_API_TOKEN"), help="API token; prefer the M5_API_TOKEN environment variable")
    commands = result.add_subparsers(dest="command", required=True)
    for name in ("status", "ping", "telemetry"):
        commands.add_parser(name)
    echo = commands.add_parser("echo", help="round-trip up to 128 UTF-8 bytes")
    echo.add_argument("data")
    wifi = commands.add_parser("wifi-set", help="save STA credentials and queue reconnect")
    wifi.add_argument("ssid")
    wifi.add_argument("--open", action="store_true", help="explicitly connect to an open STA network")
    commands.add_parser("wifi-clear", help="erase saved STA credentials; device AP remains available")
    return result


def main(argv: list[str] | None = None) -> int:
    arguments = parser()
    args = arguments.parse_args(argv)
    fields: dict[str, object] = {}
    operation = args.command
    try:
        if operation == "echo":
            if len(args.data.encode("utf-8")) > 128 or "\0" in args.data:
                raise ValueError("echo data must be at most 128 UTF-8 bytes without NUL")
            fields["data"] = args.data
        if operation in ("wifi-set", "wifi-clear"):
            if (not args.token or len(args.token) != 32 or
                    any(char not in "0123456789abcdef" for char in args.token)):
                raise ValueError("set M5_API_TOKEN (or --token) to the 32 digit API token from USB setup")
            fields["token"] = args.token
            if operation == "wifi-set":
                if not 1 <= len(args.ssid.encode("utf-8")) <= 32 or "\0" in args.ssid:
                    raise ValueError("SSID must contain 1..32 UTF-8 bytes without NUL")
                password = "" if args.open else os.environ.get("M5_WIFI_PASSWORD")
                if password is None:
                    password = getpass.getpass("STA WiFi password (empty for open network): ")
                length = len(password.encode("utf-8"))
                is_hex_psk = length == 64 and all(char in string.hexdigits for char in password)
                if "\0" in password or not (length == 0 or 8 <= length <= 63 or is_hex_psk):
                    raise ValueError("password must be empty, 8..63 UTF-8 bytes, or 64 hex digits")
                fields.update(ssid=args.ssid, password=password)
                operation = "wifi.configure"
            else:
                operation = "wifi.clear"
        payload = encode_request(operation, **fields)
        if args.ble:
            response = asyncio.run(ble_exchange(args.ble, payload, args.timeout))
            status = 200
        else:
            status, response = http_exchange(args.url, None if operation == "status" else payload, args.timeout)
        print(json.dumps(response, ensure_ascii=False, indent=2))
        return 0 if status < 400 and response.get("ok") is True else 1
    except (ValueError, RuntimeError, OSError, EOFError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
