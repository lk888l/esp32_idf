#!/usr/bin/env python3
"""Exercise CMSIS-DAP JTAG on USB, Wi-Fi TCP, or the existing BLE TCP bridge.

Run with NO target attached: this drives all debug pins, including NRST.
Open the corresponding DAP page first. Does not require test firmware.
USB needs PyUSB; TCP uses only the standard library.
"""
import argparse
import contextlib
import re
import socket
import struct
import time


def recv_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise ConnectionError("DAP connection closed")
        result.extend(part)
    return bytes(result)


class TcpDap:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def exchange(self, packet):
        self.sock.sendall(struct.pack("<4sHBB", b"DAP\0", len(packet), 1, 0) + packet)
        if packet[0] == 7:
            return b""
        magic, size, kind, reserved = struct.unpack("<4sHBB", recv_exact(self.sock, 8))
        assert magic == b"DAP\0" and 0 < size <= 64 and kind == 2 and reserved == 0
        return recv_exact(self.sock, size)

    def close(self):
        self.sock.close()


class UsbDap:
    def __init__(self):
        import usb.core
        import usb.util
        self.util = usb.util
        self.device = usb.core.find(idVendor=0x303a, idProduct=0x4004)
        if self.device is None:
            raise RuntimeError("Open USB DAP on StickS3 first")
        self.util.claim_interface(self.device, 0)

    def exchange(self, packet):
        assert self.device.write(0x01, packet, timeout=5000) == len(packet)
        if packet[0] == 7:
            return b""
        return bytes(self.device.read(0x81, 64, timeout=5000))

    def close(self):
        self.util.release_interface(self.device, 0)
        self.util.dispose_resources(self.device)


class Console:
    def __init__(self, port):
        import serial
        self.serial = serial.Serial(port, 115200, timeout=0.1)

    def command(self, command):
        self.serial.reset_input_buffer()
        self.serial.write((command + "\n").encode())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            line = self.serial.readline().decode(errors="replace")
            if "DAP TEST mode=" in line:
                return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)(?:\s|$)", line)}
        raise TimeoutError("DAP smoke console not responding; requires M5_STICKS3_DAP_SMOKE_TEST")

    def pins(self, mask):
        assert self.command("dap status")["gpio_out"] == mask

    def close(self):
        self.serial.close()


def check(dap, iterations, console=None):
    def run(*packet):
        return dap.exchange(bytes(packet))

    caps = run(0, 0xf0)
    assert caps[:2] == b"\x00\x02" and caps[2] & 3 == 3, caps.hex()
    assert run(3) == b"\x03\x00"
    assert run(2, 2) == b"\x02\x02"
    assert run(0x11, 0xa0, 0x86, 1, 0) == b"\x11\x00"  # 100 kHz
    assert run(0x15, 1, 4) == b"\x15\x00"
    if console:
        console.pins((1 << 6) | (1 << 7) | (1 << 8) | (1 << 1))
        # Internal weak pulls provide known levels on the unconnected TDO pin.
        for level, command in ((0, "dap tdo-low"), (1, "dap tdo-high")):
            console.command(command)
            reply = run(0x14, 1, 0x80, *([0xa5] * 8))
            assert reply == bytes([0x14, 0]) + bytes([0xff if level else 0]) * 8, reply.hex()
            reply = run(0x14, 1, 0x89, 0x55, 1)
            assert reply == bytes([0x14, 0, 0xff if level else 0, level]), reply.hex()
        console.command("dap tdo-float")
    # Read back actual GPIO levels for TCK, TMS and TDI. NRST is open drain.
    for value in (0, 7, 1, 2, 4):
        reply = run(0x10, value, 7, 0, 0, 0, 0)
        assert len(reply) == 2 and reply[0] == 0x10 and reply[1] & 7 == value, reply.hex()
    # TAP reset and Idle, followed by an IDCODE command (value is floating).
    assert run(0x14, 2, 0x46, 0xff, 1, 0) == b"\x14\x00"
    identity = run(0x16, 0)
    assert len(identity) == 6 and identity[:2] == b"\x16\x00", identity.hex()
    assert run(0x16, 1) == b"\x16\xff"
    for malformed in ((0x14, 1, 0x80), (0x14, 255, 1, 0), (0x15, 1, 0),
                      (0x15, 1, 33), (0x15, 9, *([4] * 9)), (0x16,)):
        assert run(*malformed) == b"\xff", malformed
    assert run(0x15, 8, *([32] * 8)) == b"\x15\x00"
    assert len(run(0x16, 7)) == 6
    assert run(0x15, 0) == b"\x15\x00"
    assert run(0x16, 0) == b"\x16\xff"
    assert run(0x15, 1, 4) == b"\x15\x00"
    # 63-byte request and 56 captured bytes cross BLE's 16-byte fragments and
    # exercise a nearly full CMSIS-DAP packet. TDO is unconstrained without a target.
    pattern = bytes([0x81, 0x42, 0x24, 0x18, 0xff, 0, 0x55, 0xaa])
    packet = bytes([0x14, 7]) + (bytes([0x80]) + pattern) * 6 + bytes([0xb0]) + pattern[:6]
    assert len(packet) == 63
    start = time.monotonic()
    for _ in range(iterations):
        reply = dap.exchange(packet)
        assert len(reply) == 56 and reply[:2] == b"\x14\x00", reply.hex()
    assert run(7) == b""
    assert run(0, 0xfe) == b"\x00\x01\x01"
    # Switch protocols on the same connection, then release every target pin.
    assert run(2, 1) == b"\x02\x01"
    if console:
        console.pins((1 << 6) | (1 << 7) | (1 << 8))
    assert run(2, 2) == b"\x02\x02"
    assert run(3) == b"\x03\x00"
    if console:
        console.pins(0)
        print("PASS: TDO low/high capture, TDO always input, TDI released on SWD switch, all pins released.")
    print(f"PASS: SWD+JTAG capabilities, GPIO readback, chain/IDCODE bounds, malformed packets,")
    print(f"      {iterations} long JTAG sequences in {time.monotonic() - start:.2f}s, protocol switch/disconnect.")
    print("No target attached: IDCODE contents, target flash/debug and electrical timing are NOT verified.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--usb", action="store_true")
    mode.add_argument("--host", help="StickS3 Wi-Fi address, or 127.0.0.1 for the BLE bridge")
    parser.add_argument("--port", type=int, default=4441)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--console", help="Optional native serial port; smoke firmware only, TCP/BLE only")
    args = parser.parse_args()
    if args.iterations < 1 or not 1 <= args.port <= 65535:
        parser.error("iterations must be positive and port must be 1..65535")
    if args.console and args.usb:
        parser.error("Native serial is unavailable while USB DAP is active")
    with contextlib.ExitStack() as stack:
        console = stack.enter_context(contextlib.closing(Console(args.console))) if args.console else None
        dap = stack.enter_context(contextlib.closing(UsbDap() if args.usb else TcpDap(args.host, args.port)))
        try:
            check(dap, args.iterations, console)
        finally:
            with contextlib.suppress(Exception):
                dap.exchange(b"\x03")


if __name__ == "__main__":
    main()
