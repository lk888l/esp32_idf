#!/usr/bin/env python3
"""Verify the normal clock firmware over USB; requires pyserial and station Wi-Fi.

Default is read-only. --exercise-wifi briefly disables/enables the radio, verifies
offline ticking and reconnection, then requests a manual sync. Saved profiles are
not edited. This script always attempts to restore Wi-Fi after disabling it.
"""
import argparse
import datetime
import json
import time

import serial


class Device:
    def __init__(self, port):
        self.port = port
        self.sequence = 20000
        self.buffer = b""

    def request(self, operation, **fields):
        self.sequence += 1
        request = {"v": 1, "id": self.sequence, "op": operation, **fields}
        self.port.write((json.dumps(request) + "\n").encode())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.buffer += self.port.read(4096)
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if any(fault in line for fault in (b"Guru Meditation", b"task_wdt", b"assert failed")):
                    raise RuntimeError("Device fault: " + line.decode(errors="replace"))
                if b"\x1e" not in line:
                    continue
                try:
                    reply = json.loads(line.split(b"\x1e", 1)[1])
                except ValueError:
                    continue
                if reply.get("id") == self.sequence:
                    return reply
        raise RuntimeError("No USB API response; use normal firmware with its console enabled")

    def query(self, operation, **fields):
        reply = self.request(operation, **fields)
        if not reply.get("ok"):
            raise RuntimeError(f"{operation}: {reply.get('error')}")
        return reply

    def wait(self, operation, predicate, timeout=90):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.query(operation)
            if predicate(last):
                return last
            time.sleep(0.5)
        raise RuntimeError(f"{operation} timed out: {last}")

    def control(self, operation):
        receipt = self.query(operation)
        ticket = receipt["ticket"]
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = self.query("command.result", ticket=ticket)
            if result["state"] == "applied":
                return
            if result["state"] == "failed":
                raise RuntimeError(f"{operation}: {result}")
            time.sleep(0.1)
        raise RuntimeError(f"{operation} command timed out")


def check_time(value):
    if abs(value["epoch"] - time.time()) > 5:
        raise RuntimeError("SNTP time differs from host by more than 5 s")
    if value["timezone"] != "CST-8" or value["interval_s"] != 3600:
        raise RuntimeError("Expected normal firmware with China timezone and hourly refresh")
    local = datetime.datetime.fromtimestamp(value["epoch"], datetime.timezone(datetime.timedelta(hours=8)))
    if value["time"] != local.strftime("%H:%M:%S") or value["date"] != local.strftime("%Y/%m/%d"):
        raise RuntimeError("Local display date/time does not match UTC+8")


def run(device, exercise_wifi):
    device.wait("wifi.status", lambda v: v["state"] == "connected")
    first = device.wait("clock.status", lambda v: v["valid"] and v["state"] == "synced")
    check_time(first)
    print(f"PASS automatic SNTP: {first['date']} {first['time']} {first['zone']}, count={first['sync_count']}", flush=True)
    if not exercise_wifi:
        return
    saved = device.query("wifi.saved")
    restore = True
    try:
        device.control("wifi.disable")
        offline = device.wait("clock.status", lambda v: not v["online"])
        start = time.monotonic()
        time.sleep(3)
        later = device.query("clock.status")
        elapsed = time.monotonic() - start
        if not later["valid"] or later["online"] or abs(later["epoch"] - offline["epoch"] - elapsed) > 2:
            raise RuntimeError("Clock did not keep ticking offline")
        if later["sync_count"] != offline["sync_count"] or device.request("clock.sync").get("error") != "wifi_not_connected":
            raise RuntimeError("Offline SNTP request was not rejected")
        print("PASS offline ticking and sync rejection", flush=True)
        device.control("wifi.enable")
        restored = device.wait("clock.status", lambda v: v["online"] and v["state"] == "synced" and v["sync_count"] > first["sync_count"])
        check_time(restored)
        restore = False
        after = device.query("wifi.saved")
        if any(after[key] != saved[key] for key in ("count", "used_mask", "preferred", "ssid")):
            raise RuntimeError("Saved Wi-Fi metadata changed")
        print("PASS automatic resync after reconnect; saved Wi-Fi preserved", flush=True)
        deadline = time.monotonic() + 15
        while True:
            reply = device.request("clock.sync")
            if reply.get("ok"):
                break
            if reply.get("error") != "busy" or time.monotonic() >= deadline:
                raise RuntimeError(f"Manual sync rejected: {reply}")
            time.sleep(0.5)
        manual = device.wait("clock.status", lambda v: v["sync_count"] > restored["sync_count"] and v["state"] == "synced")
        check_time(manual)
        print(f"PASS manual resync: count={manual['sync_count']}", flush=True)
    finally:
        if restore:
            device.control("wifi.enable")
            device.wait("wifi.status", lambda v: v["state"] == "connected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--exercise-wifi", action="store_true")
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=1) as port:
        port.write(b"\n")
        run(Device(port), args.exercise_wifi)


if __name__ == "__main__":
    main()
