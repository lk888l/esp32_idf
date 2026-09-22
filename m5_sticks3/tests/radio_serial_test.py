#!/usr/bin/env python3
"""Bounded on-device USB Serial/JTAG regression (requires pyserial).

The default checks only queries and rejected inputs. --scan-rounds enables
discovery; --power-cycle briefly disables/re-enables idle radios, restoring
their initial enable state. No mode connects to peers, changes credentials,
forgets profiles or deletes bonds. Close other serial monitors first.
Opening a serial port can reset the board through DTR/RTS; wait for startup
and retry only the initial read-only readiness probe, never mutations.
"""

from __future__ import annotations

import argparse
from collections import deque
import itertools
import json
import re
import sys
import time

import serial


class CheckFailed(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailed(message)


class Console:
    def __init__(self, port: str, timeout: float):
        self.port = serial.Serial(port, 115200, timeout=0.05, write_timeout=3)
        self.timeout = timeout
        self.buffer = bytearray()
        self.frames: deque[dict] = deque()
        self.ids = itertools.count(100_000_000)
        self.check_boot = False
        self.ui_samples = 0

    def close(self) -> None:
        self.port.close()

    def pump(self) -> None:
        self.buffer.extend(self.port.read(max(1, min(self.port.in_waiting, 4096))))
        require(len(self.buffer) <= 65536, "unterminated serial output exceeded limit")
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            if b"\x1e" in line:
                payload = line[line.index(b"\x1e") + 1:].rstrip(b"\r")
                try:
                    record = json.loads(payload)
                except (ValueError, UnicodeError) as error:
                    raise CheckFailed("damaged RS-framed response") from error
                require(isinstance(record, dict) and record.get("v") == 1,
                        "invalid response envelope")
                require(type(record.get("id")) is int and type(record.get("ok")) is bool,
                        "invalid response id/ok types")
                require(not {"password", "token", "ap_password"} & record.keys(),
                        "response exposed a secret field")
                self.frames.append(record)
            else:
                # Never echo boot logs: they contain bootstrap credentials.
                require(not re.search(rb"Guru Meditation|Backtrace:|abort\(\)|Stack canary|watchdog got triggered", line),
                        "firmware panic/watchdog detected in logs")
                if self.check_boot:
                    require(b"ESP-ROM:" not in line and b"boot: ESP-IDF" not in line,
                            "unexpected firmware reboot")
                self.ui_samples += b"display: perf " in line

    def drain(self, duration: float) -> None:
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.pump()

    def response(self, timeout: float | None = None) -> dict:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            if self.frames:
                return self.frames.popleft()
            self.pump()
        raise CheckFailed("serial response timeout (mutation, if any, was not retried)")

    def exchange(self, payload: bytes) -> dict:
        require(not self.frames, "unexpected extra response before request")
        self.port.write(payload)
        return self.response()

    def cli(self, command: str) -> dict:
        return self.exchange(command.encode("utf-8") + b"\n")

    def api(self, operation: str, **fields: object) -> dict:
        request_id = next(self.ids)
        payload = json.dumps(dict(v=1, id=request_id, op=operation, **fields),
                             ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        require(len(payload) <= 256, "test generated an oversized request")
        response = self.exchange(payload + b"\n")
        require(response["id"] == request_id, f"{operation}: response id mismatch")
        return response

    def success(self, operation: str, **fields: object) -> dict:
        response = self.api(operation, **fields)
        require(response["ok"], f"{operation}: {response.get('error')}")
        return response

    def ready(self) -> None:
        self.drain(3)
        self.frames.clear()
        self.port.write(b"\n")  # Discard an interrupted input from an earlier session.
        self.drain(0.2)
        self.frames.clear()
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                response = self.api("ping")
                if response.get("reply") == "pong":
                    self.check_boot = True
                    return
            except CheckFailed as error:
                if "timeout" not in str(error):
                    raise
            self.drain(0.3)
            self.frames.clear()
        raise CheckFailed("console did not become ready")

    def applied(self, response: dict, timeout: float = 10) -> dict:
        require(response["ok"] and response.get("result") == "accepted",
                f"mutation rejected: {response.get('error')}")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.success("command.result", ticket=response["ticket"])
            if result["state"] != "queued":
                return result
            time.sleep(0.05)
        raise CheckFailed("accepted command remained queued")

    def mutate(self, command: str) -> None:
        result = self.applied(self.cli(command))
        require(result["state"] == "applied" and result["error_code"] == 0,
                f"{command}: {result.get('error_name')}")


class Suite:
    def __init__(self, console: Console):
        self.c = console
        self.passed = 0

    def passed_case(self, label: str) -> None:
        self.passed += 1
        print(f"PASS {label}", flush=True)

    def profiles(self) -> list[dict]:
        return [{k: v for k, v in self.c.success(f"{radio}.saved", index=slot).items()
                 if k not in ("v", "id", "ok")}
                for radio in ("wifi", "ble") for slot in range(4)]

    def readonly(self) -> None:
        for command in ("help", "help wifi", "help ble", "help protocol", "capabilities",
                        "status", "wifi status", "ble status", "wifi results",
                        "ble results", "ble peer", "wifi saved", "ble saved", "ble bonds"):
            require(self.c.cli(command)["ok"], f"CLI query failed: {command}")
        caps = self.c.success("capabilities")
        require(caps["auth"] == "physical" and caps["serial_framing"] == "RS-JSON-LF",
                "wrong serial capabilities")
        require(caps["memory_slots"] == 4 and caps["queue_capacity"] == 4,
                "unexpected configured capacities")
        self.passed_case("CLI queries and USB JSON capabilities")
        before = self.c.success("traffic")
        text = '中文 Wi-Fi "quoted" \\ $;`plain`'
        require(self.c.success("echo", data=text)["data"] == text, "UTF-8 echo changed")
        after = self.c.success("traffic")
        require(after["serial_requests"] > before["serial_requests"], "serial counter stalled")
        self.passed_case("UTF-8 JSON and serial traffic counters")
        for payload in (b"ping\r", b"ping\r\n", b"\x1eping\n", b"pinx\x08g\n",
                        b"pinx\x7fg\n", "pin中\x08g\n".encode(), b"\n\r\nping\n"):
            require(self.c.exchange(payload).get("reply") == "pong", "line-ending/edit failure")
        self.passed_case("CR/LF/CRLF, RS, empty lines and UTF-8 backspace")

    def invalid(self) -> None:
        cases = (
            (b"not-a-command", "unknown_command"),
            (b'wifi connect "unfinished', "unterminated_quote"),
            (b"wifi use 4", "invalid_slot"),
            (b"ble forget -1", "invalid_slot"),
            (b"ble connect AA:BB:CC:DD:EE:FF 9", "invalid_peer"),
            (b"command 0", "invalid_ticket"),
            (b"ping extra", "usage"),
            (b"pin\x00g", "invalid_line"),
            (b"\xff", "invalid_utf8"),
            (b"x" * 513 + b"wifi off", "line_too_long"),
            (b'{"v":1,"id":21,"op":"ping"}' + b" " * 257, "request_too_large"),
            (b'{"v":1,"id":21,"id":22,"op":"ping"}', "invalid_json"),
            (b'{"v":1,"id":21,"op":"ping"}{}', "invalid_json"),
            (b'{"v":1,"id":21,"op":"wifi.use","slot":false}', "invalid_slot"),
            (b'{"v":1,"id":21,"op":"wifi.forget","slot":0,"all":true}', "invalid_slot"),
            (b'{"v":1,"id":21,"op":"wifi.disable","enabled":false}', "unknown_field"),
            (b'{"v":1,"id":21,"op":"ble.connect","remember":1}', "invalid_remember"),
        )
        for index, (payload, expected) in enumerate(cases):
            result = self.c.exchange(payload + b"\n")
            require(result["ok"] is False and result.get("error") == expected,
                    f"invalid input #{index}: expected {expected}, got {result.get('error')}")
            require(self.c.success("ping").get("reply") == "pong", "recovery failed")
        self.passed_case(f"{len(cases)} invalid inputs and complete-line quarantine/recovery")

    def scan(self, radio: str) -> None:
        before = self.c.success(f"{radio}.scan.results")
        if not before["enabled"]:
            print(f"SKIP {radio} scan: radio disabled", flush=True)
            return
        require(not before["scanning"], f"{radio} scan already running")
        self.c.mutate(f"{radio} scan")
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            data = self.c.success(f"{radio}.scan.results")
            if not data["scanning"] and data["generation"] != before["generation"]:
                break
            require(self.c.success("ping")["reply"] == "pong", "console stalled during scan")
            time.sleep(0.15)
        else:
            raise CheckFailed(f"{radio}: scan completion deadline")
        require(data["error"] == 0, f"{radio}: scan error {data['error']}")
        require(0 <= data["count"] <= 16 and data["total"] >= data["count"], "scan limits")
        for index in range(data["count"]):
            row = self.c.success(f"{radio}.scan.results", index=index)
            require(row["index"] == index and row["generation"] == data["generation"],
                    "inconsistent scan pagination")
        self.passed_case(f"{radio} async scan, {data['count']} cached / {data['total']} seen, pagination")

    def power_cycle(self, radio: str) -> None:
        state = self.c.success(f"{radio}.status")
        overall = self.c.success("status")
        scan = self.c.success(f"{radio}.scan.results")
        if radio == "wifi":
            idle = state["state"] in ("ap", "off") and scan["ap_clients"] == 0
        else:
            idle = not any(state[key] for key in ("connected", "connecting", "switching"))
            idle = idle and overall["ble"] != "connected"
        if not idle or state["remember_pending"] or scan["scanning"]:
            print(f"SKIP {radio} power cycle: active link/operation", flush=True)
            return
        original = bool(state["enabled"])
        try:
            for enabled in (False, True, False, True):
                self.c.mutate(f"{radio} {'on' if enabled else 'off'}")
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    current = self.c.success(f"{radio}.status")
                    if bool(current["enabled"]) == enabled:
                        break
                    time.sleep(0.1)
                else:
                    raise CheckFailed(f"{radio}: power state did not settle")
                if not enabled:
                    result = self.c.applied(self.c.cli(f"{radio} scan"))
                    if radio == "wifi":
                        require(result["state"] == "failed", "disabled Wi-Fi accepted a scan")
                    else:
                        # BLE tickets finish when submitted to the host queue;
                        # the host reports execution errors in scan diagnostics.
                        deadline = time.monotonic() + 3
                        while time.monotonic() < deadline:
                            diagnostic = self.c.success("ble.scan.results")
                            if not diagnostic["scanning"] and diagnostic["error"] != 0:
                                break
                            time.sleep(0.1)
                        else:
                            raise CheckFailed("disabled BLE scan did not report a host error")
                else:
                    self.scan(radio)
            self.passed_case(f"{radio} runtime off/on, disabled-scan errors and rescan")
        finally:
            self.c.mutate(f"{radio} {'on' if original else 'off'}")

    def stress(self, rounds: int) -> None:
        started = time.monotonic()
        for _ in range(rounds):
            ids = [next(self.c.ids) for _ in range(8)]
            packet = b"".join(json.dumps(dict(v=1, id=i, op="ping"), separators=(",", ":")).encode()
                              + b"\n" for i in ids)
            self.c.port.write(packet)
            for request_id in ids:
                response = self.c.response()
                require(response["id"] == request_id and response.get("reply") == "pong",
                        "burst response missing, reordered or corrupt")
        self.passed_case(f"{rounds * 8} burst JSON requests in {time.monotonic() - started:.1f}s")

    def idle_timeout(self) -> None:
        self.c.port.write(b"wifi off")
        self.c.drain(31)
        require(not self.c.frames, "partial line executed before its delimiter")
        response = self.c.exchange(b"\n")
        require(response.get("error") == "invalid_line", "expired input was not quarantined")
        require(self.c.success("ping").get("reply") == "pong", "timeout recovery failed")
        self.passed_case("30-second partial-input expiry and recovery")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--timeout", type=float, default=4)
    parser.add_argument("--scan-rounds", type=int, default=0)
    parser.add_argument("--power-cycle", action="store_true")
    parser.add_argument("--stress-rounds", type=int, default=0, help="eight ping requests per burst")
    parser.add_argument("--idle-timeout", action="store_true", help="adds a 31-second check")
    args = parser.parse_args()
    if not 0 < args.timeout <= 30 or not 0 <= args.scan_rounds <= 100 or not 0 <= args.stress_rounds <= 10000:
        parser.error("timeout must be 0..30s, scan rounds 0..100, stress rounds 0..10000")
    console = None
    try:
        console = Console(args.port, args.timeout)
        console.ready()
        suite = Suite(console)
        baseline = suite.profiles()
        bonds = console.success("ble.bonds")["count"]
        suite.readonly()
        suite.invalid()
        for _ in range(args.scan_rounds):
            for radio in ("wifi", "ble"):
                suite.scan(radio)
        if args.power_cycle:
            for radio in ("wifi", "ble"):
                suite.power_cycle(radio)
        if args.stress_rounds:
            suite.stress(args.stress_rounds)
        if args.idle_timeout:
            suite.idle_timeout()
        require(suite.profiles() == baseline, "saved profiles changed during non-persistence tests")
        require(console.success("ble.bonds")["count"] == bonds, "bond count changed")
        suite.passed_case("saved profiles/bond count unchanged; no panic or reboot")
        print(f"PASS {suite.passed} groups; {console.ui_samples} UI performance log samples", flush=True)
        return 0
    except (CheckFailed, serial.SerialException, OSError) as error:
        print(f"FAIL {error}", file=sys.stderr, flush=True)
        return 1
    finally:
        if console:
            console.close()


if __name__ == "__main__":
    raise SystemExit(main())
