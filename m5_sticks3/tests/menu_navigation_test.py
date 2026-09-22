#!/usr/bin/env python3
"""Hardware menu regression; requires pyserial and M5_STICKS3_DAP_SMOKE_TEST.

Exercises the real LVGL renderer, its configured TLSF pool, and queued navigation.
Run with the device idle on the main menu. Never selects a DAP transport.
"""

import argparse
import re
import sys
import time
from dataclasses import dataclass

import serial

CARD_COUNT = 13
STATUS = re.compile(
    r"UI TEST page=(\d+) card=(\d+) busy=(\d+) queued=(\d+) "
    r"lv_free=(\d+) lv_largest=(\d+) lv_peak=(\d+)"
)
FAULTS = ("Guru Meditation", "task_wdt", "LVGL lock timeout", "assert failed", "Rebooting...")


@dataclass
class State:
    page: int
    card: int
    busy: int
    queued: int
    free: int
    largest: int
    peak: int


class MenuTest:
    def __init__(self, port):
        self.port = port
        self.buffer = ""
        self.state = None
        self.peak = 0
        self.min_largest = None
        self.history = []

    def read_for(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.buffer += self.port.read(4096).decode(errors="replace")
            while "\n" in self.buffer:
                line, self.buffer = self.buffer.split("\n", 1)
                self.history = (self.history + [line])[-20:]
                if any(fault in line for fault in FAULTS):
                    raise RuntimeError(line.strip())
                match = STATUS.search(line)
                if match:
                    self.state = State(*map(int, match.groups()))
                    self.peak = max(self.peak, self.state.peak)
                    self.min_largest = min(self.min_largest or self.state.largest,
                                           self.state.largest)
                probe = re.search(r"DAP TEST mode=(\d+).* gpio_out=(\d+)", line)
                if probe and (int(probe[1]) or int(probe[2])):
                    raise RuntimeError("Menu browsing unexpectedly enabled the SWD probe")

    def command(self, command):
        self.state = None
        self.port.write(("dap " + command + "\n").encode())
        # Do not flush(): a stalled USB console can otherwise block indefinitely.
        self.read_for(0.15)

    def settle(self, card=None, page=0):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.command("status")
            state = self.state
            if state and state.page == page and not state.busy and not state.queued:
                if card is not None and state.card != card:
                    raise RuntimeError(f"Selected card {state.card}, expected {card}")
                return state
        raise RuntimeError("UI stopped responding or did not finish navigation; "
                           "use a DAP smoke build and start on the main menu")

    def next_card(self, card):
        self.command("next")
        card = card % CARD_COUNT + 1
        self.settle(card)
        return card

    def run(self, cycles, bursts):
        self.read_for(2)
        card = self.settle().card
        for cycle in range(cycles):
            for _ in range(CARD_COUNT):
                card = self.next_card(card)
            print(f"Menu cycle {cycle + 1}/{cycles} passed", flush=True)
        for _ in range(bursts):
            # Four presses during one transition coalesce into two steps.
            self.port.write(b"dap next\n" * 4)
            self.read_for(0.2)
            card = (card + 1) % CARD_COUNT + 1
            self.settle(card)
        for target, page in ((2, 2), (3, 3)):
            while card != target:
                card = self.next_card(card)
            self.command("select")
            self.read_for(1)
            self.settle(card, page)
            self.command("next")
            self.read_for(1)
            self.settle(card)
        print(f"PASS: {cycles * CARD_COUNT} normal presses, {bursts * 4} rapid presses, "
              f"AURA/SYSTEM round trips; LVGL peak={self.peak}, "
              f"smallest sampled free block={self.min_largest} bytes")


def positive(value):
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--cycles", type=positive, default=4)
    parser.add_argument("--bursts", type=positive, default=10)
    args = parser.parse_args()
    test = None
    try:
        with serial.Serial(args.port, 115200, timeout=0.08, write_timeout=1) as port:
            test = MenuTest(port)
            test.run(args.cycles, args.bursts)
    except (RuntimeError, serial.SerialException) as error:
        if test:
            print("\n".join(test.history), file=sys.stderr)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
