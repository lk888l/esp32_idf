#!/usr/bin/env python3
"""Verify normal StickS3 PC-monitor firmware over USB and real Wi-Fi UDP.

Requires pyserial. --exercise-wifi briefly toggles Wi-Fi and restores it even
on failure. Saved profiles are preserved; target defaults to the current STA IP.
Do not run another PC sender to the same device during this acceptance test.
"""
import argparse
import json
import math
from pathlib import Path
import socket
import time
import uuid

import serial
from clock_hardware_test import Device


def sample(session, seq):
    return {"v": 1, "session": session, "seq": seq, "host": "STICKS3-SMOKE",
            "cpu_pct": 37.5, "ram_used": 8 << 30, "ram_total": 32 << 30,
            "gpu_name": "NVIDIA SMOKE", "gpu_pct": 81.0,
            "vram_used": 4 << 30, "vram_total": 12 << 30, "gpu_temp_c": 55.0}


def run(device, host, exercise_wifi, duration):
    checks = []

    def passed(name):
        checks.append(name)
        print("PASS " + name, flush=True)

    device.wait("pc.status", lambda value: value["listening"])
    network = device.query("wifi.status")
    host = host or network["ip"]
    if not host or host == "0.0.0.0":
        raise RuntimeError("Connect station Wi-Fi or use --host for the device AP IP")
    saved = device.query("wifi.saved")
    session = uuid.uuid4().hex
    initial = device.query("pc.status")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.connect((host, 4443))
        udp.settimeout(1)

        def send(packet, expected=True):
            encoded = packet if isinstance(packet, bytes) else json.dumps(packet, separators=(",", ":")).encode()
            udp.send(encoded)
            try:
                response = json.loads(udp.recv(1024))
            except socket.timeout:
                if expected:
                    raise RuntimeError("No monitor ACK from " + host)
                return
            if not expected:
                raise RuntimeError("Invalid or duplicate sample unexpectedly acknowledged")
            assert response == {"v": 1, "session": packet["session"], "seq": packet["seq"], "ack": True}, response

        send(sample(session, 1))
        state = device.wait("pc.status", lambda value: value["session"] == session and value["seq"] == 1)
        current = device.query("pc.sample")
        assert not state["stale"] and current["cpu_pct"] == 37.5 and current["ram_used"] == 8 << 30, current
        assert current["gpu_pct"] == 81 and current["gpu_temp_c"] == 55 and current["vram_total"] == 12 << 30
        passed("Wi-Fi UDP v1 + session/sequence ACK + USB latest sample")

        send(sample(session, 1), False)
        send(sample(session, 0), False)
        for packet in (b"{}", b"x" * 1024, b"x" * 1400,
                       {**sample(session, 2), "cpu_pct": 101}, {**sample(session, 2), "v": 2}):
            send(packet, False)
        state = device.query("pc.status")
        assert state["out_of_order"] >= initial["out_of_order"] + 2 and state["rejected"] >= initial["rejected"] + 5
        assert state["seq"] == 1
        passed("duplicate / out-of-order / malformed / oversized packet rejection")

        state = device.wait("pc.status", lambda value: value["stale"], timeout=6)
        current = device.query("pc.sample")
        assert state["age_ms"] >= 3000 and all(current[key] is None for key in
            ("cpu_pct", "ram_used", "ram_total", "gpu_pct", "vram_used", "vram_total", "gpu_temp_c"))
        passed("3-second stale detection and hiding expired values")

        packet = sample(session, 2)
        for key in ("cpu_pct", "gpu_pct", "vram_used", "vram_total", "gpu_temp_c"):
            packet[key] = None
        packet["gpu_name"] = ""
        send(packet)
        device.wait("pc.status", lambda value: value["seq"] == 2 and not value["stale"])
        current = device.query("pc.sample")
        assert current["ram_total"] == 32 << 30 and current["cpu_pct"] is None and current["gpu_pct"] is None
        passed("CPU baseline / GPU-disabled null fields preserve RAM")

        session = uuid.uuid4().hex
        send(sample(session, 0xffffffff)); send(sample(session, 0))
        device.wait("pc.status", lambda value: value["session"] == session and value["seq"] == 0)
        passed("new session and u32 sequence wrap")

        if exercise_wifi:
            restore = True
            try:
                device.control("wifi.disable")
                device.wait("pc.status", lambda value: not value["listening"] and value["stale"] and not value["received"])
                device.control("wifi.enable")
                device.wait("wifi.status", lambda value: value["state"] == "connected")
                device.wait("pc.status", lambda value: value["listening"])
                address = device.query("wifi.status")["ip"]
                if network["state"] == "connected" and address != host:
                    raise RuntimeError("DHCP address changed; rerun with new IP " + address)
                send(sample(session, 1))
                device.wait("pc.status", lambda value: not value["stale"] and value["seq"] == 1)
                restore = False
                passed("Wi-Fi disable / socket release / reconnect / automatic reception")
            finally:
                if restore:
                    device.control("wifi.enable")
            after = device.query("wifi.saved")
            assert all(after[key] == saved[key] for key in ("count", "used_mask", "preferred", "ssid"))
            passed("saved Wi-Fi profiles preserved")

        start = time.monotonic()
        seq = 2
        while time.monotonic() - start < duration:
            send(sample(session, seq)); seq += 1
            time.sleep(0.1)
        final = device.query("pc.status")
        assert final["error"] == 0 and not final["stale"]
        passed(f"steady stream for {duration:g}s, final seq={final['seq']}")
        return {"host": host, "checks": checks, "initial": initial, "final": final}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--host")
    parser.add_argument("--exercise-wifi", action="store_true")
    parser.add_argument("--duration", type=float, default=15)
    parser.add_argument("--output", type=Path, default=Path("build/hardware/pc-monitor"))
    args = parser.parse_args()
    if not math.isfinite(args.duration) or args.duration <= 0:
        parser.error("--duration must be positive")
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=1) as port:
        port.write(b"\n")
        result = run(Device(port), args.host, args.exercise_wifi, args.duration)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
