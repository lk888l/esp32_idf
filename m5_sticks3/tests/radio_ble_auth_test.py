#!/usr/bin/env python3
"""Check BLE/physical-console authentication separation using an existing bond.

Linux-only, requires python3-dbus and an already paired, disconnected StickS3.
Does not scan, pair, forget or supply a valid mutation token. Only this test's
connection is disconnected afterward. An unexpectedly accepted mutation fails
immediately and is never retried.
"""

import argparse
import json
import re
import sys
import time

import dbus

from radio_api_test import MUTATIONS


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument("--adapter", default="hci0")
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", args.address):
        parser.error("invalid Bluetooth address")
    bus = dbus.SystemBus()
    path = "/org/bluez/" + args.adapter + "/dev_" + args.address.upper().replace(":", "_")
    obj = bus.get_object("org.bluez", path)
    props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
    device = dbus.Interface(obj, "org.bluez.Device1")
    require(props.Get("org.bluez.Device1", "Paired"), "target must already be paired")
    require(not props.Get("org.bluez.Device1", "Connected"), "target already connected; refusing to take it over")
    attempted = False
    try:
        attempted = True
        device.Connect(timeout=25)
        deadline = time.monotonic() + 15
        while not props.Get("org.bluez.Device1", "ServicesResolved"):
            require(time.monotonic() < deadline, "GATT service-resolution timeout")
            time.sleep(0.1)
        objects = dbus.Interface(bus.get_object("org.bluez", "/"),
                                 "org.freedesktop.DBus.ObjectManager").GetManagedObjects()
        characteristics = {}
        for object_path, interfaces in objects.items():
            char = interfaces.get("org.bluez.GattCharacteristic1")
            if str(object_path).startswith(path + "/") and char:
                characteristics[str(char["UUID"]).lower()] = dbus.Interface(
                    bus.get_object("org.bluez", object_path), "org.bluez.GattCharacteristic1")
        rx = characteristics["6e400002-b5a3-f393-e0a9-e50e24dcca9e"]
        tx = characteristics["6e400003-b5a3-f393-e0a9-e50e24dcca9e"]
        sequence = 0

        def request(operation, **fields):
            nonlocal sequence
            sequence += 1
            payload = json.dumps(dict(v=1, id=sequence, op=operation, **fields),
                                 separators=(",", ":")).encode()
            rx.WriteValue(dbus.Array(payload, signature="y"), {"type": "request"}, timeout=8)
            raw = bytes(tx.ReadValue({}, timeout=8))
            require(len(raw) <= 512, "BLE response exceeded attribute capacity")
            response = json.loads(raw)
            require(response.get("v") == 1 and response.get("id") == sequence, "BLE envelope mismatch")
            require(not {"token", "password", "ap_password"} & response.keys(), "secret field exposed")
            return response

        require(request("ping").get("reply") == "pong", "BLE ping failed")
        time.sleep(0.3)
        state = request("status")
        require(state.get("ble_secure") == 1 and state.get("ble_bonded") == 1,
                "existing encrypted bond was not restored")
        caps = request("capabilities")
        require(caps.get("auth") == "token", "BLE was granted physical-console privileges")
        print("PASS BLE encrypted existing-bond connection and token-only capabilities", flush=True)
        for operation in MUTATIONS:
            result = request(operation)
            require(result.get("ok") is False and result.get("error") == "unauthorized",
                    f"{operation}: unauthenticated BLE mutation was not rejected")
        spoofed = request("wifi.disable", origin="serial")
        require(spoofed.get("error") == "unauthorized", "JSON spoofed physical origin")
        require(request("ping").get("reply") == "pong", "BLE failed after invalid requests")
        print(f"PASS {len(MUTATIONS)} unauthenticated mutations and origin spoof rejected; bond retained", flush=True)
    finally:
        if attempted:
            device.Disconnect(timeout=10)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, dbus.DBusException, KeyError, ValueError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
