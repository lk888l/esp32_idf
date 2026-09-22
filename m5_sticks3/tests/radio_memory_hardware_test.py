#!/usr/bin/env python3
"""Opt-in connection/persistence tests against an explicitly owned target.

Requires pyserial. The selected radio's profile store must initially be empty;
existing security bonds are preserved. This suite connects/disconnects, creates
and forgets a test profile, and resets the board. Other active links are also
interrupted by reset. Never run it concurrently with a monitor or another suite.
Wi-Fi passwords come from a hidden prompt or M5_TEST_WIFI_PASSWORD, not argv.
The Wi-Fi target must be password-protected for the failed-password check.
BLE must be a connectable target in the latest scan; use its exact address type.
"""

import argparse
import getpass
import json
import os
import re
import sys
import time

from radio_serial_test import CheckFailed, Console, Suite, require


class MemorySuite(Suite):
    def wait_status(self, radio, predicate, seconds=35):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            state = self.c.success(f"{radio}.status")
            if predicate(state):
                return state
            time.sleep(0.2)
        raise CheckFailed(f"{radio}: link deadline; last status={state}")

    def reboot(self):
        self.c.check_boot = False
        self.c.frames.clear()
        self.c.buffer.clear()
        # Same hard-reset sequence as esptool.reset.HardReset(uses_usb=True).
        self.c.port.dtr = False
        self.c.port.rts = True
        time.sleep(0.2)
        self.c.port.rts = False
        time.sleep(0.2)
        self.c.ready()
        require(self.c.success("status")["accepted"] == 0, "reset did not restart the app")

    def wifi_connect(self, ssid, password, temporary=False):
        command = "wifi connect " + json.dumps(ssid, ensure_ascii=False) + " " + json.dumps(password, ensure_ascii=False)
        if temporary:
            command += " --temporary"
        result = self.c.applied(self.c.cli(command))
        # Do not include the command or password in exception messages.
        require(result["state"] == "applied", f"Wi-Fi connect: {result.get('error_name')}")
        self.wait_status("wifi", lambda s: s["state"] == "connected" and s["ssid"] == ssid
                         and not s["remember_pending"], seconds=60)

    def wifi(self, ssid, password):
        require(self.c.success("wifi.saved")["count"] == 0, "Wi-Fi store is not empty; refusing to alter existing profiles")
        self.wifi_connect(ssid, password, temporary=True)
        require(self.c.success("wifi.saved")["count"] == 0, "temporary Wi-Fi was persisted")
        self.passed_case("Wi-Fi temporary CLI connection, DHCP and no automatic memory")
        self.c.mutate("wifi remember")
        saved = self.c.success("wifi.saved")
        require(saved["count"] == 1 and saved["ssid"] == ssid, "manual Wi-Fi memory missing")
        slot = saved["preferred"]
        self.reboot()
        self.wait_status("wifi", lambda s: s["state"] == "connected" and s["ssid"] == ssid, 60)
        require(self.c.success("wifi.saved")["preferred"] == slot, "Wi-Fi preference lost across reboot")
        self.passed_case("Wi-Fi manual remember and automatic reconnect after real reset")
        # A failed replacement must not corrupt the previously working password.
        wrong_password = "M5-test-wrong-password" if password != "M5-test-wrong-password" else "M5-test-other-password"
        result = self.c.applied(self.c.success("wifi.connect", ssid=ssid,
                                              password=wrong_password, remember=True))
        require(result["state"] == "applied", "wrong-password test was not submitted")
        self.wait_status("wifi", lambda s: s["state"] != "connected" and s["reason"] != 0, 30)
        require(self.c.success("wifi.saved")["count"] == 1, "failed connection changed memory count")
        self.c.mutate(f"wifi use {slot}")
        self.wait_status("wifi", lambda s: s["state"] == "connected" and not s["remember_pending"], 60)
        self.passed_case("failed-password replacement preserved working memory; wifi use restored link")
        self.c.mutate(f"wifi forget {slot}")
        require(self.c.success("wifi.saved")["count"] == 0, "Wi-Fi forget failed")
        require(self.c.success("wifi.status")["state"] == "connected", "forget dropped the live Wi-Fi link")
        self.c.mutate("wifi disconnect")
        self.wait_status("wifi", lambda s: s["state"] == "ap")
        self.wifi_connect(ssid, password)  # CLI default remembers only after DHCP.
        require(self.c.success("wifi.saved")["ssid"] == ssid, "default Wi-Fi remember failed")
        self.passed_case("Wi-Fi forget retained link; default reconnect saved after DHCP")
        self.c.mutate("wifi forget all")  # Store contains only our explicitly created profile.
        self.c.mutate("wifi disconnect")
        self.reboot()
        require(self.c.success("wifi.saved")["count"] == 0, "forgotten Wi-Fi returned after reset")
        require(self.c.success("wifi.status")["state"] == "ap", "forgotten Wi-Fi auto-connected")
        self.passed_case("Wi-Fi forget persisted across reset; device restored to AP-only")

    def ble_connected(self):
        self.wait_status("ble", lambda s: s["connected"] and not s["connecting"]
                         and not s["switching"] and not s["remember_pending"])

    def ble(self, address, address_type):
        require(self.c.success("ble.saved")["count"] == 0, "BLE store is not empty; refusing to alter existing profiles")
        self.c.mutate(f"ble connect {address} {address_type} --temporary")
        self.ble_connected()
        require(self.c.success("ble.saved")["count"] == 0, "temporary BLE link was persisted")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            peer = self.c.success("ble.peer")
            if peer["service_count"]:
                break
            time.sleep(0.2)
        require(peer["service_count"] > 0, "BLE primary-service discovery did not complete")
        self.passed_case(f"BLE temporary outbound link and {peer['service_count']} discovered services")
        self.c.mutate("ble remember")
        saved = self.c.success("ble.saved")
        require(saved["count"] == 1, "BLE stable identity not saved")
        slot = saved["preferred"]
        self.c.mutate("ble disconnect")
        self.wait_status("ble", lambda s: not s["connected"] and not s["connecting"])
        self.c.mutate(f"ble use {slot}")
        self.ble_connected()
        self.passed_case("BLE manual memory and reconnect from saved identity")
        # A remembered selection while connected exercises the disconnect/new
        # connection path; pending save must settle on a new peer generation.
        self.c.mutate(f"ble use {slot}")
        self.ble_connected()
        self.passed_case("BLE active-link reselection completed asynchronously")
        self.reboot()
        require(self.c.success("ble.saved")["count"] == 1, "BLE memory lost across reset")
        require(not self.c.success("ble.status")["connected"], "BLE connected without explicit selection")
        self.c.mutate("ble reconnect")
        self.ble_connected()
        self.passed_case("BLE persisted across reset and explicitly reconnected")
        self.c.mutate(f"ble forget {slot}")
        require(self.c.success("ble.saved")["count"] == 0, "BLE forget failed")
        require(self.c.success("ble.status")["connected"], "forget dropped live BLE link")
        self.c.mutate("ble disconnect")
        self.wait_status("ble", lambda s: not s["connected"] and not s["connecting"])
        self.reboot()
        require(self.c.success("ble.saved")["count"] == 0, "forgotten BLE target returned after reset")
        self.passed_case("BLE forget retained live link and persisted across reset")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--wifi-ssid")
    group.add_argument("--ble-address")
    parser.add_argument("--ble-type", type=int, choices=range(4), default=0)
    args = parser.parse_args()
    password = None
    if args.wifi_ssid:
        password = os.environ.get("M5_TEST_WIFI_PASSWORD")
        if password is None:
            password = getpass.getpass("Temporary test Wi-Fi password: ")
        if not (8 <= len(password.encode("utf-8")) <= 63 or re.fullmatch(r"[0-9a-fA-F]{64}", password)):
            parser.error("this Wi-Fi test requires a protected AP with an 8..63-byte password or 64-digit hex PSK")
    c = None
    try:
        c = Console(args.port, 5)
        c.ready()
        bonds = c.success("ble.bonds")["count"]
        suite = MemorySuite(c)
        if args.wifi_ssid:
            suite.wifi(args.wifi_ssid, password)
        else:
            suite.ble(args.ble_address, args.ble_type)
        require(c.success("ble.bonds")["count"] == bonds, "existing bond count changed")
        print(f"PASS {suite.passed} connection/persistence groups; existing bonds preserved", flush=True)
        return 0
    except (CheckFailed, OSError) as error:
        print(f"FAIL {error}; inspect radio status before resuming. No existing records were erased.",
              file=sys.stderr, flush=True)
        return 1
    finally:
        if c:
            c.close()


if __name__ == "__main__":
    raise SystemExit(main())
