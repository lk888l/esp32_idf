#!/usr/bin/env python3
"""Reference client for the HexMotor ESP32-C5 wireless CAN-FD gateway."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import hmac
import json
import os
import secrets
import socket
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

MAGIC = b"HX"
PROTOCOL_VERSION = 1
HEADER_SIZE = 12
CRC_SIZE = 4
MAX_PAYLOAD_SIZE = 128
MAX_PACKET_SIZE = HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE

HELLO = 0x01
DEVICE_INFO = 0x02
AUTH_REQUEST = 0x03
AUTH_CHALLENGE = 0x04
AUTH_RESPONSE = 0x05
AUTH_RESULT = 0x06
CAN_FRAME = 0x10
STATUS_REQUEST = 0x20
STATUS_RESPONSE = 0x21
WIFI_CREDENTIALS = 0x30
WIFI_RESULT = 0x31
PING = 0x40
PONG = 0x41
ERROR = 0x7F

FLAG_RESPONSE = 1 << 0
FLAG_ERROR = 1 << 1

ORIGIN_PHYSICAL_BUS = 0
ORIGIN_LOCAL_NODE = 1
ORIGIN_WIRELESS_CLIENT = 2

DISCOVERY_REQUEST = b"HXDISC1"
DEFAULT_HOST = "192.168.4.1"
DEFAULT_TCP_PORT = 3333
DEFAULT_DISCOVERY_PORT = 3334
NONCE_SIZE = 16

KNOWN_TYPES = {
    HELLO,
    DEVICE_INFO,
    AUTH_REQUEST,
    AUTH_CHALLENGE,
    AUTH_RESPONSE,
    AUTH_RESULT,
    CAN_FRAME,
    STATUS_REQUEST,
    STATUS_RESPONSE,
    WIFI_CREDENTIALS,
    WIFI_RESULT,
    PING,
    PONG,
    ERROR,
}

TYPE_NAMES = {
    HELLO: "hello",
    DEVICE_INFO: "device_info",
    AUTH_REQUEST: "auth_request",
    AUTH_CHALLENGE: "auth_challenge",
    AUTH_RESPONSE: "auth_response",
    AUTH_RESULT: "auth_result",
    CAN_FRAME: "can_frame",
    STATUS_REQUEST: "status_request",
    STATUS_RESPONSE: "status_response",
    WIFI_CREDENTIALS: "wifi_credentials",
    WIFI_RESULT: "wifi_result",
    PING: "ping",
    PONG: "pong",
    ERROR: "error",
}

STATUS_FIELDS = (
    "wifi_state",
    "station_ipv4_raw",
    "ap_clients",
    "can_rx_frames",
    "can_rx_dropped",
    "can_tx_frames",
    "can_tx_failed",
    "can_bus_errors",
    "can_recoveries",
    "gateway_ingress_frames",
    "gateway_ingress_dropped",
    "gateway_forwarded_frames",
    "gateway_monitor_dropped",
    "wireless_ingress_packets",
    "wireless_ingress_dropped",
    "wireless_protocol_errors",
    "tcp_authenticated",
    "tcp_rx_packets",
    "tcp_tx_packets",
    "tcp_rx_invalid",
    "tcp_tx_dropped",
    "ble_connections",
    "ble_rx_packets",
    "ble_tx_packets",
    "ble_rx_invalid",
    "ble_tx_dropped",
)


class ProtocolError(RuntimeError):
    """Raised when a peer violates the HX v1 protocol."""


@dataclass(frozen=True)
class Packet:
    message_type: int
    flags: int
    sequence: int
    payload: bytes = b""


@dataclass(frozen=True)
class CanFrame:
    arbitration_id: int
    data: bytes
    extended: bool
    remote: bool
    fd: bool
    bitrate_switch: bool
    origin: int
    bus: int
    timestamp_us: int


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def encode_packet(packet: Packet) -> bytes:
    if packet.message_type not in KNOWN_TYPES:
        raise ValueError(f"unknown message type 0x{packet.message_type:02x}")
    if len(packet.payload) > MAX_PAYLOAD_SIZE:
        raise ValueError("payload exceeds 128 bytes")
    header = struct.pack(
        "<2sBBHHI",
        MAGIC,
        PROTOCOL_VERSION,
        packet.message_type,
        packet.flags,
        len(packet.payload),
        packet.sequence,
    )
    body = header + packet.payload
    return body + struct.pack("<I", crc32_ieee(body))


def decode_packet(data: bytes) -> Packet:
    if len(data) < HEADER_SIZE + CRC_SIZE:
        raise ProtocolError("packet is incomplete")
    magic, version, message_type, flags, payload_size, sequence = struct.unpack_from(
        "<2sBBHHI", data
    )
    expected_size = HEADER_SIZE + payload_size + CRC_SIZE
    if magic != MAGIC or version != PROTOCOL_VERSION:
        raise ProtocolError("invalid magic or protocol version")
    if message_type not in KNOWN_TYPES or payload_size > MAX_PAYLOAD_SIZE:
        raise ProtocolError("invalid type or payload length")
    if len(data) != expected_size:
        raise ProtocolError("packet length mismatch")
    expected_crc = struct.unpack_from("<I", data, expected_size - CRC_SIZE)[0]
    if not hmac.compare_digest(
        struct.pack("<I", crc32_ieee(data[:-CRC_SIZE])),
        struct.pack("<I", expected_crc),
    ):
        raise ProtocolError("CRC32 mismatch")
    return Packet(message_type, flags, sequence, data[HEADER_SIZE:-CRC_SIZE])


class PacketStream:
    """Incremental, resynchronizing HX packet reader for a TCP stream."""

    def __init__(self, connection: socket.socket):
        self._connection = connection
        self._buffer = bytearray()

    def receive(self, timeout: Optional[float] = None) -> Packet:
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            packet = self._extract()
            if packet is not None:
                return packet
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("timed out waiting for gateway packet")
                self._connection.settimeout(remaining)
            else:
                self._connection.settimeout(None)
            chunk = self._connection.recv(512)
            if not chunk:
                raise ConnectionError("gateway closed the TCP connection")
            self._buffer.extend(chunk)
            if len(self._buffer) > MAX_PACKET_SIZE * 6:
                raise ProtocolError("receive buffer exceeded safety limit")

    def _extract(self) -> Optional[Packet]:
        while len(self._buffer) >= 2 and self._buffer[:2] != MAGIC:
            del self._buffer[0]
        if len(self._buffer) < HEADER_SIZE:
            return None
        if self._buffer[2] != PROTOCOL_VERSION:
            del self._buffer[0]
            return None
        message_type = self._buffer[3]
        payload_size = struct.unpack_from("<H", self._buffer, 6)[0]
        if message_type not in KNOWN_TYPES or payload_size > MAX_PAYLOAD_SIZE:
            del self._buffer[0]
            return None
        total = HEADER_SIZE + payload_size + CRC_SIZE
        if len(self._buffer) < total:
            return None
        candidate = bytes(self._buffer[:total])
        try:
            packet = decode_packet(candidate)
        except ProtocolError:
            del self._buffer[0]
            return None
        del self._buffer[:total]
        return packet


def hmac_proof(key: bytes, label: bytes, client_nonce: bytes, device_nonce: bytes) -> bytes:
    return hmac.new(key, label + client_nonce + device_nonce, hashlib.sha256).digest()


def load_control_key(key_file: Optional[str]) -> bytes:
    selected = key_file or os.environ.get("HX_CONTROL_KEY_FILE")
    if selected:
        text = Path(selected).read_text(encoding="utf-8").strip()
    else:
        text = getpass.getpass("TCP control key (64 hex digits): ").strip()
    if len(text) != 64:
        raise ValueError("control key must contain exactly 64 hexadecimal digits")
    try:
        key = bytes.fromhex(text)
    except ValueError as error:
        raise ValueError("control key is not valid hexadecimal") from error
    if len(key) != 32 or not any(key):
        raise ValueError("control key must be a non-zero 256-bit value")
    return key


class GatewayClient:
    def __init__(self, host: str, port: int, key: bytes, timeout: float):
        self._connection = socket.create_connection((host, port), timeout=timeout)
        self._connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._stream = PacketStream(self._connection)
        self._sequence = secrets.randbits(32)
        self._timeout = timeout
        self._authenticate(key)

    def close(self) -> None:
        self._connection.close()

    def __enter__(self) -> "GatewayClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        return self._sequence

    def send(self, message_type: int, payload: bytes = b"", flags: int = 0) -> int:
        sequence = self.next_sequence()
        self._connection.sendall(encode_packet(Packet(message_type, flags, sequence, payload)))
        return sequence

    def receive(self, timeout: Optional[float] = None) -> Packet:
        return self._stream.receive(self._timeout if timeout is None else timeout)

    def request(self, message_type: int, expected_type: int, payload: bytes = b"") -> Packet:
        sequence = self.send(message_type, payload)
        deadline = time.monotonic() + self._timeout
        while True:
            packet = self.receive(max(0.0, deadline - time.monotonic()))
            if packet.sequence == sequence and packet.message_type in (expected_type, ERROR):
                if packet.message_type == ERROR or packet.flags & FLAG_ERROR:
                    raise ProtocolError(format_error(packet))
                return packet

    def _authenticate(self, key: bytes) -> None:
        client_nonce = secrets.token_bytes(NONCE_SIZE)
        sequence = self.send(AUTH_REQUEST, client_nonce)
        challenge = self.receive()
        if (
            challenge.message_type != AUTH_CHALLENGE
            or challenge.sequence != sequence
            or len(challenge.payload) != 64
            or challenge.payload[:NONCE_SIZE] != client_nonce
        ):
            raise ProtocolError("invalid authentication challenge")
        device_nonce = challenge.payload[NONCE_SIZE : 2 * NONCE_SIZE]
        device_proof = challenge.payload[2 * NONCE_SIZE :]
        expected_device_proof = hmac_proof(
            key, b"HX-AUTH-DEVICE-v1", client_nonce, device_nonce
        )
        if not hmac.compare_digest(device_proof, expected_device_proof):
            raise ProtocolError("gateway authentication proof is invalid")
        response_sequence = self.send(
            AUTH_RESPONSE,
            hmac_proof(key, b"HX-AUTH-CLIENT-v1", client_nonce, device_nonce),
        )
        result = self.receive()
        if (
            result.message_type != AUTH_RESULT
            or result.sequence != response_sequence
            or result.payload != b"\x01"
            or result.flags & FLAG_ERROR
        ):
            raise ProtocolError("gateway rejected client authentication")


def make_can_payload(
    arbitration_id: int,
    data: bytes,
    extended: bool,
    remote: bool,
    fd: bool,
    bitrate_switch: bool,
) -> bytes:
    if arbitration_id < 0 or arbitration_id > (0x1FFFFFFF if extended else 0x7FF):
        raise ValueError("CAN identifier is outside the selected frame format")
    if len(data) > (64 if fd else 8):
        raise ValueError("CAN payload is too long")
    if remote and fd:
        raise ValueError("CAN-FD remote frames are invalid")
    if bitrate_switch and not fd:
        raise ValueError("BRS requires a CAN-FD frame")
    flags = (
        (1 if extended else 0)
        | (2 if remote else 0)
        | (4 if fd else 0)
        | (8 if bitrate_switch else 0)
    )
    metadata = struct.pack(
        "<IBBBBQ",
        arbitration_id,
        len(data),
        flags,
        ORIGIN_WIRELESS_CLIENT,
        0,
        0,
    )
    return metadata + data


def parse_can_payload(payload: bytes) -> CanFrame:
    if len(payload) < 16:
        raise ProtocolError("CAN packet is shorter than its metadata")
    arbitration_id, size, flags, origin, bus, timestamp_us = struct.unpack_from(
        "<IBBBBQ", payload
    )
    if flags & 0xF0 or origin > ORIGIN_WIRELESS_CLIENT or len(payload) != 16 + size:
        raise ProtocolError("invalid CAN packet metadata")
    frame = CanFrame(
        arbitration_id=arbitration_id,
        data=payload[16:],
        extended=bool(flags & 1),
        remote=bool(flags & 2),
        fd=bool(flags & 4),
        bitrate_switch=bool(flags & 8),
        origin=origin,
        bus=bus,
        timestamp_us=timestamp_us,
    )
    make_can_payload(
        frame.arbitration_id,
        frame.data,
        frame.extended,
        frame.remote,
        frame.fd,
        frame.bitrate_switch,
    )
    return frame


def decode_device_info(payload: bytes) -> dict[str, object]:
    if len(payload) < 23:
        raise ProtocolError("device_info payload is incomplete")
    capabilities, uptime_ms = struct.unpack_from("<II", payload)
    node_id, wifi_state, ble_connected, version = struct.unpack_from("<BBBB", payload, 8)
    station_ipv4 = socket.inet_ntoa(payload[12:16])
    mac = ":".join(f"{byte:02X}" for byte in payload[16:22])
    version_length = payload[22]
    if len(payload) != 23 + version_length:
        raise ProtocolError("device_info firmware version length is invalid")
    firmware = payload[23:].decode("utf-8", errors="replace")
    return {
        "protocol_version": version,
        "firmware_version": firmware,
        "capabilities": f"0x{capabilities:08X}",
        "uptime_ms": uptime_ms,
        "canopen_node_id": f"0x{node_id:02X}",
        "wifi_state": wifi_state,
        "station_ipv4": station_ipv4,
        "ble_connected": bool(ble_connected),
        "mac": mac,
    }


def decode_status(payload: bytes) -> dict[str, object]:
    if len(payload) != 4 * len(STATUS_FIELDS):
        raise ProtocolError("status_response payload length is invalid")
    values = struct.unpack("<" + "I" * len(STATUS_FIELDS), payload)
    status: dict[str, object] = dict(zip(STATUS_FIELDS, values))
    raw_ip = int(status.pop("station_ipv4_raw"))
    status["station_ipv4"] = socket.inet_ntoa(struct.pack("<I", raw_ip))
    return status


def format_error(packet: Packet) -> str:
    if packet.message_type == ERROR and len(packet.payload) == 2:
        codes = {1: "invalid_packet", 2: "forbidden", 3: "queue_full", 4: "unsupported"}
        code, offending = packet.payload
        return (
            f"gateway error {codes.get(code, code)} for "
            f"{TYPE_NAMES.get(offending, hex(offending))}"
        )
    return f"gateway returned an error response of type {TYPE_NAMES.get(packet.message_type)}"


def format_can(frame: CanFrame) -> str:
    origins = ("bus", "local", "wireless")
    kind = "CAN-FD+BRS" if frame.fd and frame.bitrate_switch else "CAN-FD" if frame.fd else "CAN"
    identifier_width = 8 if frame.extended else 3
    payload = frame.data.hex(" ").upper()
    return (
        f"{frame.timestamp_us:>12} us  {origins[frame.origin]:<8} "
        f"{kind:<10} {frame.arbitration_id:0{identifier_width}X} "
        f"[{len(frame.data):02d}] {payload}"
    )


def parse_hex_bytes(text: str) -> bytes:
    normalized = text.replace(" ", "").replace(":", "").replace("-", "")
    if len(normalized) % 2:
        raise ValueError("CAN data must contain complete hexadecimal bytes")
    try:
        return bytes.fromhex(normalized)
    except ValueError as error:
        raise ValueError("CAN data is not valid hexadecimal") from error


def discover(port: int, broadcast: str, timeout: float) -> list[dict[str, object]]:
    found: dict[tuple[str, bytes], dict[str, object]] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.bind(("", 0))
        udp.settimeout(timeout)
        udp.sendto(DISCOVERY_REQUEST, (broadcast, port))
        deadline = time.monotonic() + timeout
        while True:
            udp.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                reply, source = udp.recvfrom(128)
            except socket.timeout:
                break
            if len(reply) != 24 or reply[:7] != DISCOVERY_REQUEST:
                continue
            expected_crc = struct.unpack_from("<I", reply, 20)[0]
            if crc32_ieee(reply[:20]) != expected_crc:
                continue
            version = reply[7]
            tcp_port, capabilities = struct.unpack_from("<HI", reply, 8)
            mac_bytes = reply[14:20]
            result = {
                "address": source[0],
                "tcp_port": tcp_port,
                "protocol_version": version,
                "capabilities": f"0x{capabilities:08X}",
                "mac": ":".join(f"{byte:02X}" for byte in mac_bytes),
            }
            found[(source[0], mac_bytes)] = result
    return list(found.values())


def connect_from_args(args: argparse.Namespace) -> GatewayClient:
    return GatewayClient(
        args.host,
        args.port,
        load_control_key(args.key_file),
        args.timeout,
    )


def command_discover(args: argparse.Namespace) -> None:
    results = discover(args.discovery_port, args.broadcast, args.timeout)
    print(json.dumps(results, indent=2, ensure_ascii=False))
    if not results:
        raise RuntimeError("no gateway replied to UDP discovery")


def command_info(args: argparse.Namespace) -> None:
    with connect_from_args(args) as client:
        response = client.request(HELLO, DEVICE_INFO)
        print(json.dumps(decode_device_info(response.payload), indent=2))


def command_status(args: argparse.Namespace) -> None:
    with connect_from_args(args) as client:
        response = client.request(STATUS_REQUEST, STATUS_RESPONSE)
        print(json.dumps(decode_status(response.payload), indent=2))


def command_monitor(args: argparse.Namespace) -> None:
    with connect_from_args(args) as client:
        print("Authenticated; monitoring CAN/CAN-FD frames (Ctrl-C to stop).")
        while True:
            packet = client.receive(None)
            if packet.message_type == CAN_FRAME:
                print(format_can(parse_can_payload(packet.payload)), flush=True)
            elif packet.message_type == ERROR:
                print(format_error(packet), file=sys.stderr, flush=True)


def command_send(args: argparse.Namespace) -> None:
    arbitration_id = int(args.can_id, 0)
    extended = args.extended or arbitration_id > 0x7FF
    data = parse_hex_bytes(args.data)
    payload = make_can_payload(
        arbitration_id,
        data,
        extended,
        args.remote,
        args.fd,
        args.brs,
    )
    expected_data = data
    with connect_from_args(args) as client:
        client.send(CAN_FRAME, payload)
        deadline = time.monotonic() + args.timeout
        while True:
            packet = client.receive(max(0.0, deadline - time.monotonic()))
            if packet.message_type == ERROR:
                raise ProtocolError(format_error(packet))
            if packet.message_type != CAN_FRAME:
                continue
            frame = parse_can_payload(packet.payload)
            if (
                frame.origin == ORIGIN_WIRELESS_CLIENT
                and frame.arbitration_id == arbitration_id
                and frame.data == expected_data
                and frame.fd == args.fd
            ):
                print(format_can(frame))
                return


def command_selftest(_: argparse.Namespace) -> None:
    packet = Packet(PING, 0x1234, 0x89ABCDEF, b"wireless-selftest")
    encoded = encode_packet(packet)
    if decode_packet(encoded) != packet:
        raise AssertionError("packet round-trip failed")
    corrupted = bytearray(encoded)
    corrupted[-1] ^= 0x80
    try:
        decode_packet(bytes(corrupted))
    except ProtocolError:
        pass
    else:
        raise AssertionError("CRC corruption was not detected")
    can_payload = make_can_payload(0x18FF50E5, bytes(range(12)), True, False, True, True)
    can_frame = parse_can_payload(can_payload)
    if can_frame.arbitration_id != 0x18FF50E5 or can_frame.data != bytes(range(12)):
        raise AssertionError("CAN-FD round-trip failed")
    print("wireless_client protocol self-test passed")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="HexMotor ESP32-C5 secure wireless CAN/CAN-FD gateway client"
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="gateway IPv4 address")
    parser.add_argument("--port", type=int, default=DEFAULT_TCP_PORT, help="gateway TCP port")
    parser.add_argument(
        "--key-file",
        help="file containing only the 64-hex-digit TCP control key "
        "(or set HX_CONTROL_KEY_FILE)",
    )
    parser.add_argument("--timeout", type=float, default=3.0, help="network timeout in seconds")
    subparsers = parser.add_subparsers(dest="command", required=True)

    discovery = subparsers.add_parser("discover", help="find gateways via UDP broadcast")
    discovery.add_argument("--discovery-port", type=int, default=DEFAULT_DISCOVERY_PORT)
    discovery.add_argument("--broadcast", default="255.255.255.255")
    discovery.set_defaults(function=command_discover)

    info = subparsers.add_parser("info", help="authenticate and read device information")
    info.set_defaults(function=command_info)

    status = subparsers.add_parser("status", help="authenticate and read gateway counters")
    status.set_defaults(function=command_status)

    monitor = subparsers.add_parser("monitor", help="stream CAN/CAN-FD monitor frames")
    monitor.set_defaults(function=command_monitor)

    send = subparsers.add_parser("send", help="send one CAN/CAN-FD frame")
    send.add_argument("can_id", help="CAN identifier, for example 0x321")
    send.add_argument("data", nargs="?", default="", help="payload bytes in hexadecimal")
    send.add_argument("--extended", action="store_true", help="use a 29-bit identifier")
    send.add_argument("--remote", action="store_true", help="send an RTR frame")
    send.add_argument("--fd", action="store_true", help="send a CAN-FD frame")
    send.add_argument("--brs", action="store_true", help="enable CAN-FD bitrate switching")
    send.set_defaults(function=command_send)

    selftest = subparsers.add_parser("selftest", help="run offline protocol checks")
    selftest.set_defaults(function=command_selftest)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        function: Callable[[argparse.Namespace], None] = args.function
        function(args)
        return 0
    except KeyboardInterrupt:
        return 130
    except (ConnectionError, OSError, ProtocolError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

