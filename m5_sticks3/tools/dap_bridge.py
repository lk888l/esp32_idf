#!/usr/bin/env python3
"""StickS3 BLE <-> OpenOCD CMSIS-DAP TCP bridge and read-only probe checks.

Python 3.11+. BLE commands require `pip install bleak` (use a virtualenv).
The listener binds to 127.0.0.1 and accepts one debugger. No flash writes occur
unless the attached debugger sends them. A broken/timed-out BLE transaction
closes the TCP session; it is never automatically replayed.
"""
import argparse
import asyncio
import contextlib
import struct
import sys

PACKET_SIZE = 64
HEADER = struct.Struct("<4sHBB")
SERVICE_UUID = "8d5d0001-673a-4c8e-a791-005041443373"
RX_UUID = "8d5d0002-673a-4c8e-a791-005041443373"
TX_UUID = "8d5d0003-673a-4c8e-a791-005041443373"


def frame(payload: bytes, kind: int = 1) -> bytes:
    if not 0 < len(payload) <= PACKET_SIZE or kind not in (1, 2):
        raise ValueError("Invalid DAP packet size or type")
    return HEADER.pack(b"DAP\0", len(payload), kind, 0) + payload


async def read_frame(reader, kind=1, timeout=None):
    # An idle debugger can remain stopped indefinitely. Once a header starts,
    # partial headers and payloads must complete within a bounded interval.
    first = await asyncio.wait_for(reader.readexactly(1), timeout) if timeout else await reader.readexactly(1)
    rest = await asyncio.wait_for(reader.readexactly(HEADER.size - 1), 5)
    signature, size, received_kind, reserved = HEADER.unpack(first + rest)
    if signature != b"DAP\0" or received_kind != kind or reserved or not 0 < size <= PACKET_SIZE:
        raise ValueError("Malformed CMSIS-DAP TCP header")
    return await asyncio.wait_for(reader.readexactly(size), 5)


def fragments(packet: bytes, sequence: int, mtu: int):
    if not 1 <= len(packet) <= PACKET_SIZE or mtu < 23:
        raise ValueError("Invalid packet or ATT MTU")
    chunk = min(mtu - 7, PACKET_SIZE)
    for offset in range(0, len(packet), chunk):
        yield struct.pack("<HBB", sequence, offset, len(packet)) + packet[offset:offset + chunk]


def response_packet(value: bytes, sequence: int):
    if len(value) < 5:
        raise ValueError("Short BLE response")
    received_sequence, ready, size = struct.unpack_from("<HBH", value)
    if ready not in (0, 1) or size > PACKET_SIZE or len(value) != size + 5:
        raise ValueError("Malformed BLE response")
    if received_sequence != sequence or not ready:
        return None
    return value[5:]


class BleDap:
    def __init__(self, client, timeout=5):
        self.client = client
        self.sequence = 0
        self.timeout = timeout
        # BlueZ cannot expose the negotiated MTU through this GATT API.
        # Use its guaranteed minimum instead of private APIs or noisy warnings.
        self.mtu = 23 if sys.platform.startswith("linux") else max(23, client.mtu_size)

    async def exchange(self, packet):
        self.sequence = (self.sequence + 1) & 0xffff
        async with asyncio.timeout(self.timeout):
            # Linux may report 23 even with a larger negotiated MTU. Small
            # fragments are valid in both cases and keep this path portable.
            for part in fragments(packet, self.sequence, self.mtu):
                await self.client.write_gatt_char(RX_UUID, part, response=True)
            if packet[0] == 0x07:  # TransferAbort has no response.
                return b""
            while True:
                value = bytes(await self.client.read_gatt_char(TX_UUID))
                result = response_packet(value, self.sequence)
                if result is not None:
                    if not result or result[0] not in (packet[0], 0xff):
                        raise ValueError("Mismatched DAP command response")
                    return result
                await asyncio.sleep(0.01)


async def relay(reader, writer, exchange):
    while True:
        packet = await read_frame(reader)
        result = await exchange(packet)
        if result:
            writer.write(frame(result, 2))
            await asyncio.wait_for(writer.drain(), 5)


async def run_bridge(args):
    from bleak import BleakClient
    busy = False
    closing = False
    idle = asyncio.Event()
    idle.set()
    failed = asyncio.Event()
    loop = asyncio.get_running_loop()
    active_writer = None

    def lost(_client):
        loop.call_soon_threadsafe(failed.set)

    # Pair before opening the TCP listener: OpenOCD's initial DAP_Info timeout
    # must not include user interaction in the operating-system pairing dialog.
    print("Connecting BLE; open B-DAP and complete system pairing...", flush=True)
    async with BleakClient(args.address, pair=True, timeout=30, disconnected_callback=lost) as client:
        if client.services.get_service(SERVICE_UUID) is None:
            raise RuntimeError("DAP service missing; refresh the OS GATT cache after installing this firmware")
        transport = BleDap(client, args.timeout)
        await transport.exchange(bytes([3]))

        async def handle(reader, writer):
            nonlocal busy, closing, active_writer
            # A debugger can reconnect while the preceding TCP EOF is still
            # releasing SWD over BLE. Wait only for that bounded cleanup;
            # another genuinely active debugger is still rejected immediately.
            if busy and closing:
                try:
                    await asyncio.wait_for(idle.wait(), args.timeout)
                except TimeoutError:
                    writer.close()
                    await writer.wait_closed()
                    return
            if busy or failed.is_set():
                writer.close()
                await writer.wait_closed()
                return
            busy = True
            idle.clear()
            active_writer = writer
            try:
                print("Debugger connected", flush=True)
                await relay(reader, writer, transport.exchange)
            except asyncio.IncompleteReadError:
                closing = True
                print("Debugger disconnected", flush=True)
            except Exception as error:
                print(f"Session failed: {type(error).__name__}: {error}", flush=True)
                failed.set()
            finally:
                # A clean TCP close releases the SWD pins and leaves BLE ready
                # for another host session. Failure tears down BLE entirely.
                if not failed.is_set() and client.is_connected:
                    try:
                        await transport.exchange(bytes([3]))
                    except Exception:
                        failed.set()
                writer.close()
                with contextlib.suppress(Exception):
                    await writer.wait_closed()
                active_writer = None
                busy = closing = False
                idle.set()

        server = await asyncio.start_server(handle, "127.0.0.1", args.port, limit=4096)
        print(f"Ready: CMSIS-DAP 127.0.0.1:{args.port} -> BLE {args.address}", flush=True)
        print("Start OpenOCD with tools/openocd/sticks3-ble.cfg", flush=True)
        async with server:
            await failed.wait()
        if active_writer:
            active_writer.close()
    raise ConnectionError("BLE session ended; reopen B-DAP and restart the bridge before reconnecting OpenOCD")


async def info_tcp(args):
    reader, writer = await asyncio.wait_for(asyncio.open_connection(args.host, args.port), 5)
    try:
        for identifier, name in ((1, "Vendor"), (2, "Product"), (3, "Serial"), (4, "CMSIS-DAP"),
                                 (0xf0, "Capabilities"), (0xfe, "Packet count"), (0xff, "Packet size")):
            writer.write(frame(bytes([0, identifier])))
            await writer.drain()
            response = await read_frame(reader, 2, timeout=5)
            if len(response) < 2 or response[0] != 0 or response[1] != len(response) - 2:
                raise ValueError("Invalid DAP_Info response")
            value = response[2:]
            print(f"{name}: {value.rstrip(bytes([0])).decode('ascii') if identifier < 0x10 else int.from_bytes(value, 'little')}")
    finally:
        writer.close()
        await writer.wait_closed()


async def scan(_args):
    from bleak import BleakScanner
    for device in await BleakScanner.discover(timeout=8):
        print(f"{device.address}  {device.name or '(unnamed)'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    ble = commands.add_parser("ble", help="Bridge encrypted BLE to local CMSIS-DAP TCP")
    ble.add_argument("--address", required=True, help="BLE address or macOS device UUID from scan")
    ble.add_argument("--port", type=int, default=4441)
    ble.add_argument("--timeout", type=float, default=5)
    ble.set_defaults(run=run_bridge)
    info = commands.add_parser("info", help="Read DAP identity over Wi-Fi or the local BLE bridge")
    info.add_argument("--host", default="192.168.4.1")
    info.add_argument("--port", type=int, default=4441)
    info.set_defaults(run=info_tcp)
    commands.add_parser("scan", help="List nearby BLE addresses").set_defaults(run=scan)
    args = parser.parse_args()
    if hasattr(args, "port") and not 1 <= args.port <= 65535:
        parser.error("Port must be between 1 and 65535")
    if hasattr(args, "timeout") and args.timeout <= 0:
        parser.error("Timeout must be positive")
    try:
        asyncio.run(args.run(args))
    except KeyboardInterrupt:
        pass
    except TimeoutError:
        parser.exit(1, "Connection or DAP command timed out; check the DAP page and system pairing. "
                       "On headless Linux, pair with bluetoothctl first.\n")
    except ImportError as error:
        parser.exit(2, f"{error}; install BLE dependency with: pip install bleak\n")
    except Exception as error:
        parser.exit(1, f"{type(error).__name__}: {error}\n")


if __name__ == "__main__":
    main()
