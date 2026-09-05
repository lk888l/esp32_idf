#!/usr/bin/env python3
"""HTTP regression checks for a running M5-StickS3 API v1 device.

The default suite is read-only and sends no valid authentication token.
--exercise-wifi explicitly enables destructive STA configuration tests: connect
through the device AP, because saved STA credentials are cleared before testing
and again in finally. Cleanup attempts to restore AP-only mode and verifies it.
Set M5_API_TOKEN to enable these optional tests without shell-history exposure.
"""

from __future__ import annotations

import argparse
import itertools
import math
import os
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from connectivity_client import encode_request, http_exchange, positive_timeout


class CheckFailed(RuntimeError):
    pass


class Suite:
    def __init__(self, url: str, timeout: float):
        self.url = url
        self.timeout = timeout
        self.passed = 0
        self.ids = itertools.count(int(time.time()) % 1_000_000_000)

    @staticmethod
    def require(condition: bool, message: str) -> None:
        if not condition:
            raise CheckFailed(message)

    def passed_case(self, label: str) -> None:
        self.passed += 1
        print(f"PASS {label}")

    def exchange(self, payload: bytes | None) -> tuple[int, dict]:
        status, response = http_exchange(self.url, payload, self.timeout)
        self.require(response.get("v") == 1, "response has wrong/missing API version")
        self.require(type(response.get("id")) is int, "response has wrong/missing request ID")
        self.require(type(response.get("ok")) is bool, "response has wrong/missing success flag")
        return status, response

    def success(self, operation: str, **fields: object) -> dict:
        request_id = next(self.ids)
        status, response = self.exchange(encode_request(operation, request_id, **fields))
        self.require(status == 200 and response["ok"], f"{operation} did not succeed")
        self.require(response["id"] == request_id, f"{operation} response ID mismatch")
        return response

    def error(self, label: str, payload: bytes, error: str, status_code: int = 200) -> None:
        status, response = self.exchange(payload)
        self.require(status == status_code, f"{label}: unexpected HTTP status")
        self.require(response["ok"] is False, f"{label}: invalid request was accepted")
        self.require(response.get("error") == error, f"{label}: unexpected error code")
        self.passed_case(label)

    def read_only(self) -> None:
        status, state = self.exchange(None)
        self.require(status == 200 and state["ok"], "GET status failed")
        for key in ("wifi", "ip", "ap", "ap_ip", "ble", "mtu", "completed", "last_id", "last_error"):
            self.require(key in state, f"status is missing {key}")
        self.require("token" not in state and "password" not in state, "status exposed secret fields")
        self.passed_case("GET status")
        self.success("status")
        self.passed_case("POST status")
        self.require(self.success("ping").get("reply") == "pong", "ping reply mismatch")
        self.passed_case("ping")
        echo = "M5 / \u4f60\u597d / \"quoted\" / \\ / \n"
        self.require(self.success("echo", data=echo).get("data") == echo, "echo round-trip mismatch")
        self.passed_case("UTF-8 and escaped echo")
        self.require(self.success("echo", data="x" * 128).get("data") == "x" * 128,
                     "128 byte echo boundary mismatch")
        self.passed_case("maximum echo size")
        telemetry = self.success("telemetry")
        for key in ("valid", "samples", "roll", "pitch", "yaw"):
            value = telemetry.get(key)
            self.require(isinstance(value, (int, float)) and math.isfinite(value),
                         f"telemetry has invalid {key}")
        self.passed_case("telemetry snapshot")

        self.error("malformed JSON", b'{"v":1', "invalid_json")
        self.error("trailing data", b'{"v":1,"id":1,"op":"ping"} extra', "invalid_json")
        self.error("duplicate keys", b'{"v":1,"id":1,"op":"ping","op":"status"}', "invalid_json")
        self.error("escaped duplicate keys", b'{"v":1,"id":1,"op":"ping","\\u006fp":"status"}', "invalid_json")
        self.error("escaped NUL", b'{"v":1,"id":1,"op":"echo","data":"\\u0000"}', "invalid_json")
        self.error("raw embedded NUL", b'{"v":1,"id":1,"op":"ping"}\0', "embedded_nul", 400)
        self.error("excessive nesting", b'{"v":1,"id":1,"op":"ping","extra":[[[[[]]]]]}', "invalid_json")
        self.error("wrong version", b'{"v":2,"id":1,"op":"ping"}', "invalid_envelope")
        for label, value in (("missing", None), ("negative", "-1"),
                             ("fractional", "1.5"), ("overflow", "2147483648"),
                             ("boolean", "true")):
            body = b'{"v":1,"op":"ping"}' if value is None else (
                '{"v":1,"id":' + value + ',"op":"ping"}').encode("ascii")
            self.error(f"{label} request ID", body, "invalid_envelope")
        self.error("unknown operation", encode_request("unknown"), "unknown_operation")
        self.error("missing echo data", encode_request("echo"), "invalid_data")
        self.error("oversized echo data", encode_request("echo", data="x" * 129), "invalid_data")
        self.error("missing authentication", encode_request("wifi.clear"), "unauthorized")
        self.error("wrong authentication", encode_request("wifi.clear", token="invalid"), "unauthorized")
        self.error("empty HTTP body", b"", "empty_request", 400)
        self.error("oversized HTTP body", b" " * 257, "request_too_large", 413)
        self.require(self.success("ping").get("reply") == "pong", "device did not recover after invalid traffic")
        self.passed_case("healthy after invalid requests")

    def wait_completed(self, request_id: int) -> dict:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            status = self.success("status")
            if status.get("last_id") == request_id:
                self.require(status.get("last_error") == 0, "queued WiFi command completed with an error")
                return status
            time.sleep(0.1)
        raise CheckFailed("queued WiFi command did not complete before timeout")

    def wifi_command(self, operation: str, token: str, **fields: object) -> dict:
        response = self.success(operation, token=token, **fields)
        self.require(response.get("result") == "accepted", "WiFi command acknowledgement missing")
        return self.wait_completed(response["id"])

    def exercise_wifi(self, token: str) -> None:
        try:
            state = self.wifi_command("wifi.clear", token)
            self.require(state.get("wifi") == "ap", "wifi.clear did not select AP-only mode")
            self.passed_case("authenticated STA clear and completion")
            self.error("authenticated invalid credentials",
                       encode_request("wifi.configure", next(self.ids), token=token,
                                      ssid="", password="12345678"), "invalid_credentials")
            # Valid credentials for a deliberately absent SSID exercise asynchronous
            # reconfiguration without requiring the user's LAN password.
            state = self.wifi_command("wifi.configure", token,
                                      ssid="M5-test-absent-" + uuid.uuid4().hex[:8],
                                      password="test-passphrase")
            self.require(state.get("wifi") in ("connecting", "retry"),
                         "absent STA network did not enter connection/retry state")
            self.require(bool(state.get("ap_ip")), "fallback AP address disappeared")
            self.passed_case("absent STA configuration and completion")
        finally:
            failed_before_cleanup = sys.exc_info()[0] is not None
            try:
                state = self.wifi_command("wifi.clear", token)
                self.require(state.get("wifi") == "ap", "cleanup did not restore AP-only mode")
                self.passed_case("finally clears test STA credentials")
            except (ValueError, RuntimeError, OSError) as error:
                if not failed_before_cleanup:
                    raise
                print(f"Cleanup also failed: {error}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://192.168.4.1", help="device HTTP base URL")
    parser.add_argument("--timeout", type=positive_timeout, default=15.0, help="request/completion timeout in seconds")
    parser.add_argument("--token", default=os.environ.get("M5_API_TOKEN"), help="prefer M5_API_TOKEN; used only with --exercise-wifi")
    parser.add_argument("--exercise-wifi", action="store_true", help="explicitly clear saved STA credentials and test reconfiguration; use the device AP")
    args = parser.parse_args(argv)
    if args.exercise_wifi and (not args.token or len(args.token) != 32 or
                              any(char not in "0123456789abcdef" for char in args.token)):
        parser.error("--exercise-wifi requires M5_API_TOKEN (or --token) from USB setup")
    suite = Suite(args.url, args.timeout)
    try:
        suite.read_only()
        if args.exercise_wifi:
            suite.exercise_wifi(args.token)
        print(f"All {suite.passed} connectivity API checks passed.")
        return 0
    except (ValueError, RuntimeError, OSError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
