#!/usr/bin/env python3
"""StickS3 BLE gateway over USB serial, HTTP or encrypted BLE.

USB requires pyserial; BLE requires bleak. Set M5_API_TOKEN for wireless access.
Writes are submitted once. A timeout never triggers an automatic write retry.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import secrets
import sys
import time

from connectivity_client import (MAX_RESPONSE_BYTES, RX_UUID, TX_UUID,
                                 decode_response, encode_request, http_exchange,
                                 positive_timeout)


class GatewayError(RuntimeError):
    pass


class RecordFramer:
    """Bounded RS/JSON/LF extraction; boot logs and their secrets are discarded."""
    def __init__(self):
        self.record = None

    def feed(self, chunk: bytes) -> list[dict]:
        frames = []
        for byte in chunk:
            if byte == 0x1e:
                self.record = bytearray()
            elif self.record is not None:
                if byte == 10:
                    payload, self.record = bytes(self.record).rstrip(b"\r"), None
                    frames.append(decode_response(payload))
                else:
                    self.record.append(byte)
                    if len(self.record) > MAX_RESPONSE_BYTES:
                        self.record = None
                        raise GatewayError("serial record exceeds protocol limit")
        return frames


class Endpoint:
    def __init__(self, args):
        self.args, self.serial, self.ble = args, None, None
        self.framer = RecordFramer()
        self.request_id = secrets.randbelow(1_000_000_000) + 1

    async def __aenter__(self):
        try:
            if self.args.port:
                try:
                    import serial
                except ImportError as error:
                    raise GatewayError("USB requires: python -m pip install pyserial") from error
                self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=3)
                self.serial.dtr = self.serial.rts = False
                self.serial.port = self.args.port
                self.serial.open()
                self.serial.write(b"\n")
                deadline = time.monotonic() + self.args.timeout
                # Some hosts reset on CDC open. Retry only a read-only readiness
                # probe before dispatching any of the user's operations.
                while True:
                    try:
                        await self.call("ping", timeout=min(1.0, max(0.05, deadline - time.monotonic())))
                        break
                    except (GatewayError, TimeoutError):
                        if time.monotonic() >= deadline:
                            raise GatewayError("USB console did not become ready") from None
                        await asyncio.sleep(0.1)
            elif self.args.ble:
                try:
                    from bleak import BleakClient
                except ImportError as error:
                    raise GatewayError("BLE requires: python -m pip install bleak") from error
                self.ble = BleakClient(self.args.ble, timeout=self.args.timeout, pair=True)
                try:
                    await self.ble.connect()
                except Exception as error:
                    raise GatewayError(f"BLE connection failed ({type(error).__name__})") from error
            return self
        except BaseException:
            await self.__aexit__(None, None, None)
            raise

    async def __aexit__(self, *_):
        if self.serial:
            self.serial.close()
        if self.ble and self.ble.is_connected:
            try:
                await self.ble.disconnect()
            except Exception as error:
                raise GatewayError(f"BLE cleanup failed ({type(error).__name__}); do not repeat a completed write") from error

    async def exchange(self, payload, request_id, timeout):
        if self.serial:
            if self.serial.write(payload + b"\n") != len(payload) + 1:
                raise GatewayError("partial USB write; request was not retried")
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                for frame in self.framer.feed(self.serial.read(max(1, min(self.serial.in_waiting, 1024)))):
                    if frame.get("id") == request_id:
                        return frame
                await asyncio.sleep(0)
            raise GatewayError("USB response timeout; request was not retried")
        if self.ble:
            async def transaction():
                await self.ble.write_gatt_char(RX_UUID, payload, response=True)
                return decode_response(bytes(await self.ble.read_gatt_char(TX_UUID)))
            try:
                return await asyncio.wait_for(transaction(), timeout)
            except Exception as error:
                raise GatewayError(
                    f"BLE exchange failed ({type(error).__name__}); verify encryption and control-link MTU; request was not retried"
                ) from error
        status, reply = await asyncio.to_thread(http_exchange, self.args.url, payload, timeout)
        if status >= 400 and reply.get("ok") is True:
            raise GatewayError(f"unexpected HTTP status {status}")
        return reply

    async def call(self, operation, timeout=None, **fields):
        self.request_id = self.request_id % 2_000_000_000 + 1
        if not self.args.port:
            token = self.args.token
            if not token or len(token) != 32 or any(c not in "0123456789abcdef" for c in token):
                raise GatewayError("set M5_API_TOKEN to the 32 digit token from local USB setup")
            fields["token"] = token
        request_id = self.request_id
        reply = await self.exchange(encode_request(operation, request_id, **fields), request_id, timeout or self.args.timeout)
        if (type(reply.get("v")) is not int or reply["v"] != 1 or type(reply.get("id")) is not int or
                reply["id"] != request_id or type(reply.get("ok")) is not bool):
            raise GatewayError("response envelope or request ID mismatch")
        if not reply["ok"]:
            raise GatewayError(f"{operation}: {reply.get('error', 'failed')}")
        return reply


async def wait_result(endpoint, ticket, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = await endpoint.call("ble.gatt.result", ticket=ticket,
                                     timeout=max(0.05, deadline - time.monotonic()))
        if result.get("state") == "failed":
            raise GatewayError(f"GATT failed: {result.get('error_name', result.get('error_domain'))} ({result.get('error_code')})")
        if result.get("state") == "complete":
            return result
        if result.get("state") not in ("queued", "running"):
            raise GatewayError("invalid operation state")
        await asyncio.sleep(0.1)
    raise GatewayError(f"ticket {ticket} timed out; operation was not retried")


async def collect_value(endpoint, first, operation, **identity):
    length = first.get("length")
    if type(length) is not int or not 0 <= length <= 512:
        raise GatewayError("invalid value length")
    value, page = bytearray(), first
    while True:
        if (page.get("length") != length or page.get("offset") != len(value) or
                any(page.get(key) != expected for key, expected in identity.items())):
            raise GatewayError("value changed during pagination")
        try:
            chunk = bytes.fromhex(page["data"])
        except (KeyError, TypeError, ValueError) as error:
            raise GatewayError("invalid hex value") from error
        if len(chunk) > 64 or len(value) + len(chunk) > length or (not chunk and len(value) < length):
            raise GatewayError("invalid value page")
        value.extend(chunk)
        if len(value) == length:
            return bytes(value)
        fields = {k: v for k, v in identity.items() if operation != "ble.gatt.result" or k == "ticket"}
        page = await endpoint.call(operation, offset=len(value), **fields)


def display(value):
    print(json.dumps(value, ensure_ascii=False, separators=(",", ":")), flush=True)


async def run(args):
    async with Endpoint(args) as endpoint:
        if args.command in ("scan", "results", "connect", "disconnect"):
            operation = {"scan": "ble.scan", "results": "ble.scan.results",
                         "connect": "ble.connect", "disconnect": "ble.disconnect"}[args.command]
            fields = {}
            if args.command == "results":
                fields["index"] = args.index
            if args.command == "connect":
                fields.update(address=args.address, address_type=args.address_type, remember=args.remember)
            display(await endpoint.call(operation, **fields))
            return
        status = await endpoint.call("ble.gatt.status")
        if args.command == "status":
            display(status)
            return
        if not status["connected"]:
            raise GatewayError("connect the external BLE peripheral first")
        generation = args.generation if args.generation is not None else status["generation"]
        if generation != status["generation"]:
            raise GatewayError("external connection generation does not match")
        if args.command == "watch":
            deadline, cursor = time.monotonic() + args.seconds, args.after
            while time.monotonic() < deadline:
                event = await endpoint.call("ble.gatt.events", generation=generation, after=cursor)
                if event["found"]:
                    value = await collect_value(endpoint, event, "ble.gatt.events",
                                                generation=generation, sequence=event["sequence"])
                    event["data"] = value.hex()
                    display(event)
                    cursor = event["sequence"]
                else:
                    await asyncio.sleep(0.1)
            return
        fields = {"generation": generation}
        if hasattr(args, "handle"):
            fields["handle"] = args.handle
        if hasattr(args, "end"):
            fields["end"] = args.end
        if args.command == "write":
            value = bytes.fromhex(args.data)
            if len(value) > min(status["write_bytes"], status["mtu"] - 3):
                raise GatewayError("write exceeds current limit; negotiate MTU or use application-specific chunks")
            fields["data"] = value.hex()
        if args.command == "subscribe":
            fields["mode"] = {"off": 0, "notify": 1, "indicate": 2}[args.mode]
        receipt = await endpoint.call("ble.gatt." + args.command, **fields)
        ticket = receipt["ticket"]
        result = await wait_result(endpoint, ticket, args.timeout)
        if result["generation"] != generation:
            raise GatewayError("result belongs to another connection")
        if result["count"]:
            rows = []
            for index in range(result["count"]):
                page = result if index == 0 else await endpoint.call("ble.gatt.result", ticket=ticket, index=index)
                if page.get("generation") != generation or page.get("ticket") != ticket:
                    raise GatewayError("discovery result changed")
                rows.append({k: page[k] for k in ("uuid", "handle", "end", "value_handle", "properties")})
            display({"ticket": ticket, "generation": generation, "rows": rows,
                     "total": result["total"], "truncated": result["truncated"]})
        else:
            result["data"] = (await collect_value(endpoint, result, "ble.gatt.result",
                                                 ticket=ticket, generation=generation)).hex()
            display(result)


def handle_number(text):
    value = int(text, 0)
    if not 1 <= value <= 65535:
        raise argparse.ArgumentTypeError("handle must be 1..65535 (decimal or 0x hex)")
    return value


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    transport = p.add_mutually_exclusive_group()
    transport.add_argument("--port", help="USB serial port, COMx or /dev/ttyACMx")
    transport.add_argument("--url", default="http://192.168.4.1")
    transport.add_argument("--ble", help="StickS3 control-link BLE address")
    p.add_argument("--timeout", type=positive_timeout, default=30.0)
    p.add_argument("--generation", type=int, help="require this external connection generation")
    p.set_defaults(token=os.environ.get("M5_API_TOKEN"))
    commands = p.add_subparsers(dest="command", required=True)
    for name in ("status", "scan", "disconnect", "services", "mtu", "pair"):
        commands.add_parser(name)
    results = commands.add_parser("results")
    results.add_argument("--index", type=int, default=0)
    connect = commands.add_parser("connect")
    connect.add_argument("address")
    connect.add_argument("--address-type", type=int, choices=range(4), required=True)
    connect.add_argument("--remember", action="store_true")
    for name in ("characteristics", "descriptors", "read", "write", "subscribe"):
        cmd = commands.add_parser(name)
        cmd.add_argument("handle", type=handle_number, help="value/CCCD handle, or discovery start")
        if name in ("characteristics", "descriptors"):
            cmd.add_argument("end", type=handle_number)
        if name == "write":
            cmd.add_argument("data", help="hexadecimal bytes, e.g. 0100ff")
        if name == "subscribe":
            cmd.add_argument("mode", choices=("off", "notify", "indicate"))
    watch = commands.add_parser("watch")
    watch.add_argument("--seconds", type=positive_timeout, default=60)
    watch.add_argument("--after", type=int, default=0)
    return p


def main(argv=None):
    try:
        asyncio.run(run(parser().parse_args(argv)))
        return 0
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
