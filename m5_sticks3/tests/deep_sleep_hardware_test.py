#!/usr/bin/env python3
"""Exercise the real L2 sleep app using M5_STICKS3_DAP_SMOKE_TEST firmware.

Requires pyserial and saved station Wi-Fi for initial SNTP. Each cycle uses the
smoke-only five-second timer or a normal sleep mode, reconnects native USB, and
checks RTC restoration before the next network response. No Wi-Fi settings or
DAP transports are changed.
"""

import argparse
import datetime
import math
import os
import re
import sys
import time
from collections import deque
from pathlib import Path

import serial
from serial.tools import list_ports


UI = re.compile(r"UI TEST page=(\d+) card=(\d+) busy=(\d+) queued=(\d+)")
CLOCK = re.compile(
    r"CLOCK TEST state=(\S+) valid=(\d+) online=(\d+) epoch=(\d+) "
    r"count=(\d+) interval=(\d+) error=(-?\d+) menu_time=(\S+)"
)
ENTER = re.compile(r"L2 deep sleep: timer=(\d+) s keys=11,12 rtc_valid=(\d+) epoch=(\d+)")
WAKE = re.compile(r"wake cause=(\d+) keys=0x([0-9a-fA-F]+) epoch=(\d+)")
RESTORE = re.compile(r"RTC restored after deep sleep: epoch=(\d+); sync age=(\d+) s")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
FAULTS = (
    "Guru Meditation", "task_wdt", "LVGL lock timeout", "assert failed",
    "Rebooting...", "sleep canceled:", "module teardown failed:",
    "BMI270 not found", "failed to start BMI270", "failed to start motion task",
)


class SleepTest:
    def __init__(self, requested_port, log=None):
        self.requested_port = requested_port
        self.identity = next((p for p in list_ports.comports()
                              if os.path.realpath(p.device) == os.path.realpath(requested_port)), None)
        self.port = None
        self.buffer = ""
        self.state = self.clock = None
        self.history = deque(maxlen=35)
        self.events = []
        self.disconnects = 0
        self.log = log

    def close(self):
        if self.port:
            self.port.close()
            self.port = None

    def candidates(self):
        ports = list(list_ports.comports())
        if self.identity:
            matching = [p for p in ports if (p.vid, p.pid) == (self.identity.vid, self.identity.pid)
                        and (p.serial_number == self.identity.serial_number if self.identity.serial_number
                             else p.location == self.identity.location)]
            return [p.device for p in sorted(matching, key=lambda p: p.device != self.requested_port)]
        return [self.requested_port] if os.path.exists(self.requested_port) else []

    def connect(self):
        if self.port:
            return True
        for name in self.candidates():
            port = serial.Serial(port=None, baudrate=115200, timeout=0.04, write_timeout=1)
            # Linux ACM asserts both lines on open. Clearing DTR first creates
            # DTR=0/RTS=1, which resets native ESP32-S3 USB Serial/JTAG.
            # Match esp-idf-monitor: open with both asserted, clear RTS first.
            port.rts = port.dtr = True
            port.port = name
            try:
                port.open()
                port.rts = False
                port.dtr = False
            except (serial.SerialException, OSError):
                port.close()
                continue
            self.port = port
            self.buffer = ""
            return True
        return False

    def event(self, name, **values):
        self.events.append({"kind": name, "at": time.monotonic(), **values})

    def line(self, raw):
        line = ANSI.sub("", raw).strip()
        # Some configurations print local AP credentials during startup.
        # Never keep those bytes in a test log or failure report.
        if re.search(r"(?i)(?:password|token|secret)\s*[=:]", line):
            line = "[credential log redacted]"
        self.history.append(line)
        if self.log:
            self.log.write(line + "\n")
            self.log.flush()
        if any(fault in line for fault in FAULTS):
            raise RuntimeError(line)
        if match := UI.search(line):
            self.state = dict(zip(("page", "card", "busy", "queued"), map(int, match.groups())))
        if match := CLOCK.search(line):
            self.clock = {"state": match[1], "valid": int(match[2]), "online": int(match[3]),
                          "epoch": int(match[4]), "count": int(match[5]), "interval": int(match[6]),
                          "error": int(match[7]), "menu_time": match[8]}
        if match := ENTER.search(line):
            self.event("enter", timer=int(match[1]), valid=int(match[2]), epoch=int(match[3]))
        if match := WAKE.search(line):
            self.event("wake", cause=int(match[1]), keys=int(match[2], 16), epoch=int(match[3]))
        if match := RESTORE.search(line):
            self.event("restore", epoch=int(match[1]), age=int(match[2]))
        if "SNTP synchronized:" in line:
            self.event("sntp")
        if "first fused sample after" in line:
            self.event("motion")
        if probe := re.search(r"DAP TEST mode=(\d+).* gpio_out=(\d+)", line):
            if int(probe[1]) or int(probe[2]):
                raise RuntimeError("Sleep/menu actions unexpectedly enabled the DAP probe")

    def read_for(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not self.connect():
                time.sleep(0.02)
                continue
            try:
                data = self.port.read(4096)
            except (serial.SerialException, OSError):
                self.disconnects += 1
                self.close()
                self.buffer = ""
                time.sleep(0.02)
                continue
            self.buffer += data.decode(errors="replace")
            while "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                self.line(line)

    def command(self, command):
        deadline = time.monotonic() + 5
        while not self.connect():
            if time.monotonic() >= deadline:
                raise RuntimeError("USB did not reconnect for command " + command)
            self.read_for(0.1)
        self.port.write(("dap " + command + "\n").encode())
        self.read_for(0.2)

    def settle(self, page=0, card=None, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.state = self.clock = None
            self.command("status")
            state = self.state
            if state and state["page"] == page and not state["busy"] and not state["queued"]:
                if card is None or state["card"] == card:
                    return state
        raise RuntimeError(f"UI did not settle at page={page}, card={card}; last={self.state}")

    def wait_event(self, kind, start, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            event = next((e for e in self.events[start:] if e["kind"] == kind), None)
            if event:
                return event
            self.read_for(0.1)
        raise RuntimeError("Missing device log: " + kind)

    def check_header(self):
        # The main label refreshes once a second, so a nearby second is valid.
        value = self.clock
        if not value or not value["valid"] or value["interval"] <= 0:
            raise RuntimeError("Main screen did not recover a valid clock")
        zone = datetime.timezone(datetime.timedelta(hours=8))
        nearby = {datetime.datetime.fromtimestamp(value["epoch"] + delta, zone).strftime("%H:%M:%S")
                  for delta in (-1, 0, 1)}
        if value["menu_time"] not in nearby:
            raise RuntimeError("Main header time does not match the current UTC+8 clock")
        return value.copy()

    def cancel_countdown(self):
        start = len(self.events)
        disconnects = self.disconnects
        self.command("sleep")
        self.settle(page=11, card=15)
        self.command("sleep-mode")
        self.command("sleep-start")
        self.command("next")
        self.settle(page=11, card=15)
        self.read_for(3.5)
        self.settle(page=11, card=15)
        if self.disconnects != disconnects or any(e["kind"] == "enter" for e in self.events[start:]):
            raise RuntimeError("Cancelled countdown still entered deep sleep")
        self.command("next")
        self.settle(page=0, card=15)
        print("PASS countdown cancellation and return to menu", flush=True)

    def motion_round_trip(self):
        state = self.settle(page=0)
        for _ in range(16):
            if state["card"] == 1:
                break
            self.command("next")
            state = self.settle(page=0)
        start = len(self.events)
        self.command("select")
        self.settle(page=1, card=1)
        self.wait_event("motion", start, timeout=15)
        self.command("next")
        self.settle(page=0, card=1)

    def run(self, cycles, sleep_seconds=5, max_drift_seconds=3.0):
        self.read_for(1)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            self.settle(page=0)
            if self.clock and self.clock["valid"] and self.clock["state"] == "synced":
                break
            self.read_for(0.5)
        else:
            raise RuntimeError("Initial SNTP sync unavailable; saved station Wi-Fi is required")
        self.read_for(1.1)
        self.settle(page=0)
        self.check_header()
        self.cancel_countdown()
        for cycle in range(cycles):
            self.settle(page=0)
            before = self.check_header()
            start = len(self.events)
            if sleep_seconds == 5:
                self.command("sleep-test")
            else:
                self.command("sleep")
                self.settle(page=11, card=15)
                for _ in range((300, 1800, 3600).index(sleep_seconds) + 1):
                    self.command("sleep-mode")
                self.command("sleep-start")
            self.settle(page=11, card=15)
            entered = self.wait_event("enter", start)
            if entered["timer"] != sleep_seconds or not entered["valid"]:
                raise RuntimeError(f"Expected {sleep_seconds}-second L2 sleep with a valid RTC")
            wake = self.wait_event("wake", start, timeout=sleep_seconds + 30)
            restored = self.wait_event("restore", start)
            if wake["cause"] != 4 or wake["keys"] != 0:
                raise RuntimeError(f"Expected timer wake cause=4, got {wake}")
            network = next((e for e in self.events[start:]
                            if e["kind"] == "sntp" and e["at"] >= wake["at"]), None)
            if network and network["at"] < restored["at"]:
                raise RuntimeError("RTC was restored only after a network time response")
            elapsed = restored["at"] - entered["at"]
            advance = restored["epoch"] - entered["epoch"]
            gain = advance - elapsed
            print(f"MEASURE cycle {cycle + 1}/{cycles}: timer={sleep_seconds} s, "
                  f"host={elapsed:.2f} s, RTC=+{advance} s, gain={gain:+.2f} s", flush=True)
            if (advance < sleep_seconds - 1 or
                    restored["epoch"] <= before["epoch"]):
                raise RuntimeError(f"RTC did not advance through sleep: {advance} s over {elapsed:.2f} s")
            if abs(gain) > max_drift_seconds:
                raise RuntimeError(f"RTC drift {gain:+.2f} s exceeds {max_drift_seconds:g} s tolerance")
            self.settle(page=0, timeout=20)
            self.read_for(1.1)
            self.settle(page=0)
            after = self.check_header()
            resynced_after_wake = any(e["kind"] == "sntp" and e["at"] >= restored["at"]
                                      for e in self.events[start:])
            if (after["count"] < before["count"] or
                    (after["epoch"] < restored["epoch"] and
                     (after["count"] <= before["count"] or not resynced_after_wake))):
                raise RuntimeError("Clock metadata or current time regressed after wake")
            self.motion_round_trip()
            print(f"PASS cycle {cycle + 1}/{cycles}: timer wake, RTC +{advance} s before SNTP, "
                  f"header={after['menu_time']}, motion recovered", flush=True)
        self.cancel_countdown()
        print(f"PASS {cycles} actual L2 sleep/wake cycles; main screen restored", flush=True)


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def positive_float(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a positive finite number")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--cycles", type=positive, default=3)
    parser.add_argument("--sleep-seconds", type=int, choices=(5, 300, 1800, 3600), default=5,
                        help="RTC sleep timer duration (default: 5)")
    parser.add_argument("--max-drift-seconds", type=positive_float, default=3.0,
                        help="maximum absolute RTC gain over host time (default: 3)")
    parser.add_argument("--log", type=Path, help="save redacted device logs")
    args = parser.parse_args()
    log = args.log.open("w") if args.log else None
    test = SleepTest(args.port, log)
    try:
        test.run(args.cycles, args.sleep_seconds, args.max_drift_seconds)
    except (RuntimeError, serial.SerialException, OSError) as error:
        print("\n".join(test.history), file=sys.stderr)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        test.close()
        if log:
            log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
