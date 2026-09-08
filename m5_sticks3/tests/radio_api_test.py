#!/usr/bin/env python3
"""HTTP regression checks for the M5-StickS3 radio analyzer.

The default suite reads diagnostics and sends rejected requests without a token.
It never changes saved credentials, radio power, or peer connections.

--scan additionally requests one scan per enabled radio and verifies asynchronous
completion and result pages. No nearby device is required. Leave the device
controls idle during this test so another command cannot replace last_id.
Only read requests receive bounded retries; mutations are sent exactly once.
"""

from __future__ import annotations

import argparse
import http.client
import itertools
import os
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from connectivity_client import encode_request, http_exchange, positive_timeout

UINT32_MAX = (1 << 32) - 1
ADDRESS = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\Z")
SERVICE_UUID = re.compile(
    r"(?:0x[0-9a-fA-F]{4}|0x[0-9a-fA-F]{8}|"
    r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})\Z"
)
PAGED_OPERATIONS = ("wifi.scan.results", "ble.scan.results", "ble.peer")
MUTATIONS = (
    "wifi.configure", "wifi.clear", "wifi.scan", "wifi.reconnect",
    "wifi.disconnect", "wifi.enable", "wifi.disable",
    "ble.scan", "ble.connect", "ble.disconnect", "ble.enable", "ble.disable",
)


class CheckFailed(RuntimeError):
    pass


class Suite:
    def __init__(self, url: str, timeout: float, read_retries: int, scan_timeout: float):
        self.url = url
        self.timeout = timeout
        self.read_retries = read_retries
        self.scan_timeout = scan_timeout
        self.ids = itertools.count(int(time.time()) % 1_000_000_000)
        self.passed = 0
        self.retried = 0
        self.skipped = 0

    @staticmethod
    def require(condition: bool, message: str) -> None:
        if not condition:
            raise CheckFailed(message)

    def passed_case(self, label: str) -> None:
        self.passed += 1
        print(f"PASS {label}", flush=True)

    def integer(self, data: dict, key: str, low: int, high: int) -> int:
        value = data.get(key)
        self.require(type(value) is int and low <= value <= high,
                     f"{key} must be an integer in [{low}, {high}]")
        return value

    def string(self, data: dict, key: str, max_bytes: int) -> str:
        value = data.get(key)
        self.require(isinstance(value, str), f"{key} must be a string")
        self.require("\0" not in value and len(value.encode("utf-8")) <= max_bytes,
                     f"{key} is not a bounded UTF-8 string")
        return value

    def address(self, data: dict, key: str, allow_empty: bool = False) -> str:
        value = self.string(data, key, 17)
        self.require((allow_empty and value == "") or ADDRESS.fullmatch(value) is not None,
                     f"{key} must use AA:BB:CC:DD:EE:FF format")
        return value

    def exchange(self, payload: bytes, label: str, *, readonly: bool,
                 deadline: float | None = None) -> tuple[int, dict]:
        attempts = 1 + (self.read_retries if readonly else 0)
        for attempt in range(attempts):
            remaining = self.timeout if deadline is None else deadline - time.monotonic()
            self.require(remaining > 0, f"{label}: polling deadline exceeded")
            try:
                status, response = http_exchange(self.url, payload, min(self.timeout, remaining))
            except (RuntimeError, OSError, http.client.HTTPException) as error:
                if attempt + 1 >= attempts:
                    suffix = "" if readonly else "; mutation was not retried and may already be running"
                    raise CheckFailed(f"{label}: transport failed ({type(error).__name__}){suffix}") from error
                self.retried += 1
                print(f"RETRY read {label} ({attempt + 1}/{self.read_retries}, "
                      f"{type(error).__name__})", flush=True)
                delay = min(0.4 * (attempt + 1), 1.0)
                if deadline is not None:
                    delay = min(delay, max(0.0, deadline - time.monotonic()))
                time.sleep(delay)
                continue
            # Protocol errors are not transient transport errors and are never retried.
            self.require(type(response.get("v")) is int and response["v"] == 1,
                         f"{label}: invalid API version")
            self.require(type(response.get("id")) is int, f"{label}: invalid response ID")
            self.require(type(response.get("ok")) is bool, f"{label}: invalid success flag")
            self.require(not {"token", "password", "ap_password"} & response.keys(),
                         f"{label}: secret fields exposed")
            return status, response
        raise CheckFailed(f"{label}: no response")

    def success(self, operation: str, *, readonly: bool = True,
                deadline: float | None = None, **fields: object) -> dict:
        request_id = next(self.ids)
        status, response = self.exchange(
            encode_request(operation, request_id, **fields), operation,
            readonly=readonly, deadline=deadline)
        self.require(status == 200 and response["ok"], f"{operation}: request did not succeed")
        self.require(response["id"] == request_id, f"{operation}: response ID mismatch")
        return response

    def error(self, label: str, payload: bytes, expected: str, *, readonly: bool,
              request_id: int | None = None) -> None:
        status, response = self.exchange(payload, label, readonly=readonly)
        self.require(status == 200 and response["ok"] is False,
                     f"{label}: invalid request was accepted or HTTP status changed")
        self.require(response.get("error") == expected, f"{label}: unexpected error code")
        if request_id is not None:
            self.require(response["id"] == request_id, f"{label}: response ID mismatch")
        self.passed_case(label)

    def validate_scan(self, radio: str, data: dict, index: int = 0) -> None:
        for key in ("enabled", "scanning"):
            self.integer(data, key, 0, 1)
        self.integer(data, "generation", 0, UINT32_MAX)
        count = self.integer(data, "count", 0, 16)
        self.integer(data, "total", count, 65535)
        self.integer(data, "error", -(1 << 31), (1 << 31) - 1)
        if radio == "wifi":
            self.integer(data, "current_channel", 0, 14)
            self.integer(data, "ap_clients", 0, 2)
        if count == 0:
            self.require("index" not in data, f"{radio}: empty scan unexpectedly contains a row")
            return
        self.require(data.get("index") == index, f"{radio}: wrong page index")
        self.integer(data, "rssi", -128, 127)
        if radio == "wifi":
            self.string(data, "ssid", 32)  # Empty names are valid hidden SSIDs.
            self.address(data, "bssid")
            self.integer(data, "channel", 1, 14)
            self.integer(data, "auth", 0, 255)  # Native wifi_auth_mode_t, serialized as uint8_t.
        else:
            self.string(data, "name", 31)  # Unnamed advertisements are valid.
            self.address(data, "address")
            self.integer(data, "address_type", 0, 3)
            self.integer(data, "connectable", 0, 1)

    def validate_peer(self, data: dict) -> None:
        for key in ("enabled", "connecting", "connected"):
            self.integer(data, key, 0, 1)
        self.address(data, "address", allow_empty=True)
        self.string(data, "name", 31)
        self.integer(data, "rssi", -128, 127)
        self.integer(data, "mtu", 23, 517)
        self.integer(data, "error", -(1 << 31), (1 << 31) - 1)
        count = self.integer(data, "service_count", 0, 8)
        if count:
            self.require(data.get("index") == 0, "BLE peer: wrong service page index")
            service = self.string(data, "service", 39)
            self.require(SERVICE_UUID.fullmatch(service) is not None, "BLE peer: invalid service UUID")
        else:
            self.require("service" not in data and "index" not in data,
                         "BLE peer: empty service list unexpectedly contains a row")

    def validate_traffic(self, data: dict) -> None:
        for key in ("http_requests", "ble_requests", "rx_bytes", "tx_bytes"):
            self.integer(data, key, 0, UINT32_MAX)
        self.require(self.string(data, "transport", 4) in ("", "HTTP", "BLE"),
                     "traffic: unknown transport")
        self.string(data, "operation", 23)
        self.string(data, "echo", 64)
        self.integer(data, "request_id", 0, (1 << 31) - 1)
        self.integer(data, "success", 0, 1)

    def read_only(self) -> None:
        for radio in ("wifi", "ble"):
            self.validate_scan(radio, self.success(f"{radio}.scan.results"))
            self.passed_case(f"{radio} diagnostic fields and first page")
        self.validate_peer(self.success("ble.peer"))
        self.passed_case("BLE peer fields and service page")
        before = self.success("traffic")
        self.validate_traffic(before)
        self.require(self.success("ping").get("reply") == "pong", "ping reply mismatch")
        after = self.success("traffic")
        self.validate_traffic(after)
        self.require((after["http_requests"] - before["http_requests"]) & UINT32_MAX >= 2,
                     "traffic: HTTP request count did not advance")
        for key in ("rx_bytes", "tx_bytes"):
            self.require((after[key] - before[key]) & UINT32_MAX > 0,
                         f"traffic: {key} did not advance")
        self.passed_case("traffic fields and application-byte counters")

        for operation in PAGED_OPERATIONS:
            for label, index in (("negative", -1), ("fractional", 0.5),
                                 ("string", "0"), ("out of range", 255)):
                request_id = next(self.ids)
                self.error(f"{operation} rejects {label} index",
                           encode_request(operation, request_id, index=index),
                           "invalid_index", readonly=True, request_id=request_id)
        for operation in MUTATIONS:
            request_id = next(self.ids)
            # Deliberately never add args.token. An unexpected acceptance stops
            # the suite immediately, and this mutation-shaped request is not retried.
            self.error(f"{operation} requires authentication",
                       encode_request(operation, request_id), "unauthorized",
                       readonly=False, request_id=request_id)
        for label, invalid in (("invalid byte", b"\xff"), ("overlong", b"\xc0\xaf"),
                               ("surrogate", b"\xed\xa0\x80"), ("truncated", b"\xe4\xb8")):
            body = b'{"v":1,"id":1,"op":"echo","data":"' + invalid + b'"}'
            self.error(f"rejects {label} UTF-8", body, "invalid_json",
                       readonly=True, request_id=0)
        self.require(self.success("ping").get("reply") == "pong",
                     "device did not recover after rejected requests")
        self.passed_case("healthy after analyzer validation checks")

    def wait_idle(self, radio: str, deadline: float) -> dict:
        while time.monotonic() < deadline:
            data = self.success(f"{radio}.scan.results", deadline=deadline)
            self.validate_scan(radio, data)
            if not data["scanning"]:
                return data
            time.sleep(0.25)
        raise CheckFailed(f"{radio}: existing scan did not finish before polling deadline")

    def wait_completed(self, request_id: int, baseline: dict, deadline: float) -> None:
        while time.monotonic() < deadline:
            state = self.success("status", deadline=deadline)
            for key in ("accepted", "completed", "last_id"):
                self.integer(state, key, 0, UINT32_MAX)
            self.integer(state, "last_error", -(1 << 31), (1 << 31) - 1)
            if state["last_id"] == request_id:
                self.require(state["last_error"] == 0,
                             f"scan command completed with error {state['last_error']}")
                for key in ("accepted", "completed"):
                    self.require((state[key] - baseline[key]) & UINT32_MAX > 0,
                                 f"scan command: {key} counter did not advance")
                return
            time.sleep(0.25)
        raise CheckFailed("scan command completion was not observed; another command may have replaced last_id")

    def scan(self, radio: str, token: str) -> None:
        before = self.wait_idle(radio, time.monotonic() + self.scan_timeout)
        if not before["enabled"]:
            self.skipped += 1
            print(f"SKIP {radio} scan: radio is off; this suite does not enable it", flush=True)
            return
        baseline = self.success("status")
        for key in ("accepted", "completed"):
            self.integer(baseline, key, 0, UINT32_MAX)
        # Send exactly once, even if the acknowledgement is lost as Wi-Fi
        # leaves the AP channel. A timeout cannot safely authorize a resend.
        accepted = self.success(f"{radio}.scan", readonly=False, token=token)
        self.require(accepted.get("result") == "accepted", f"{radio}: acknowledgement missing")
        deadline = time.monotonic() + self.scan_timeout
        self.wait_completed(accepted["id"], baseline, deadline)
        self.passed_case(f"{radio} scan acknowledgement and queued completion")
        while time.monotonic() < deadline:
            data = self.success(f"{radio}.scan.results", deadline=deadline)
            self.validate_scan(radio, data)
            if data["generation"] != before["generation"] and not data["scanning"]:
                self.require(data["error"] == 0, f"{radio}: scan failed with error {data['error']}")
                break
            time.sleep(0.25)
        else:
            raise CheckFailed(f"{radio}: completed scan generation was not observed")
        rows = [data] if data["count"] else []
        for index in range(1, data["count"]):
            row = self.success(f"{radio}.scan.results", index=index, deadline=deadline)
            self.validate_scan(radio, row, index)
            self.require(not row["scanning"] and row["generation"] == data["generation"] and
                         row["count"] == data["count"], f"{radio}: scan changed during pagination")
            rows.append(row)
        stable = self.success(f"{radio}.scan.results", deadline=deadline)
        self.require(not stable["scanning"] and stable["generation"] == data["generation"] and
                     stable["count"] == data["count"], f"{radio}: scan changed after pagination")
        self.require(all(left["rssi"] >= right["rssi"] for left, right in zip(rows, rows[1:])),
                     f"{radio}: result pages are not sorted by descending RSSI")
        identities = [(row["bssid"].upper(),) if radio == "wifi" else
                      (row["address"].upper(), row["address_type"]) for row in rows]
        self.require(len(identities) == len(set(identities)), f"{radio}: duplicate result addresses")
        self.passed_case(f"{radio} scan generation and {len(rows)} bounded, sorted result pages")


def retry_count(value: str) -> int:
    result = int(value)
    if not 0 <= result <= 5:
        raise argparse.ArgumentTypeError("read retries must be between 0 and 5")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://192.168.4.1", help="device HTTP base URL")
    parser.add_argument("--timeout", type=positive_timeout, default=6.0, help="per-request timeout in seconds")
    parser.add_argument("--read-retries", type=retry_count, default=2, help="extra read attempts after transport failures (0..5)")
    parser.add_argument("--scan-timeout", type=positive_timeout, default=45.0, help="bounded scan/completion polling time in seconds")
    parser.add_argument("--scan", action="store_true", help="scan enabled radios once; never change credentials or connections")
    parser.add_argument("--token", default=os.environ.get("M5_API_TOKEN"), help="used only with --scan; prefer M5_API_TOKEN")
    args = parser.parse_args(argv)
    if args.scan and (not args.token or len(args.token) != 32 or
                      any(char not in "0123456789abcdef" for char in args.token)):
        parser.error("--scan requires the 32 digit M5_API_TOKEN (or --token) from USB setup")
    suite = Suite(args.url, args.timeout, args.read_retries, args.scan_timeout)
    try:
        suite.read_only()
        if args.scan:
            for radio in ("wifi", "ble"):
                suite.scan(radio, args.token)
        print(f"All {suite.passed} radio API checks passed; "
              f"{suite.retried} read retries, {suite.skipped} disabled-radio scans skipped.")
        return 0
    except (ValueError, RuntimeError, OSError, http.client.HTTPException) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
