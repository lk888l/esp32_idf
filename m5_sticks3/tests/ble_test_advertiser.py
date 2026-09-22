#!/usr/bin/env python3
"""Temporary Linux BLE peripheral for StickS3 connection/discovery tests.

Requires system python3-dbus/python3-gi and an already powered BlueZ adapter.
Exports only a connectable advertisement; BlueZ provides its existing GAP/GATT
services. Never changes adapter power, alias, discovery or pairing records.
SIGINT/SIGTERM or the bounded lifetime removes this application's advertisement.
Reference: https://bluez.readthedocs.io/en/latest/advertising-api/
"""

import argparse
import signal
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

ADV_IFACE = "org.bluez.LEAdvertisement1"


class Advertisement(dbus.service.Object):
    def __init__(self, bus, loop, name):
        self.path = "/org/m5sticks3/test/advertisement"
        super().__init__(bus, self.path)
        self.loop = loop
        self.registered = False
        self.name = name

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != ADV_IFACE:
            raise dbus.exceptions.DBusException("Unknown interface", name="org.bluez.Error.InvalidArguments")
        return {"Type": "peripheral", "LocalName": self.name,
                "ServiceUUIDs": dbus.Array(["180a"], signature="s"),
                "Discoverable": dbus.Boolean(True)}

    @dbus.service.method(ADV_IFACE, in_signature="", out_signature="")
    def Release(self):
        self.registered = False
        self.loop.quit()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--name", default="M5-Radio-Test-PC")
    parser.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 1 <= len(args.name.encode()) <= 20:
        parser.error("lifetime must be 1..3600 seconds and name 1..20 UTF-8 bytes")
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    adapter = bus.get_object("org.bluez", "/org/bluez/" + args.adapter)
    properties = dbus.Interface(adapter, "org.freedesktop.DBus.Properties")
    if not properties.Get("org.bluez.Adapter1", "Powered"):
        parser.error("adapter is off; this test will not change its power state")
    manager = dbus.Interface(adapter, "org.bluez.LEAdvertisingManager1")
    loop = GLib.MainLoop()
    advertisement = Advertisement(bus, loop, args.name)
    failed = []

    def registered():
        advertisement.registered = True
        print(f"READY name={args.name} adapter={args.adapter} "
              f"identity={properties.Get('org.bluez.Adapter1', 'Address')}", flush=True)

    def error(reason):
        failed.append(str(reason))
        print(f"FAIL {reason}", file=sys.stderr, flush=True)
        loop.quit()

    def stop(*_):
        loop.quit()
        return False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    GLib.timeout_add_seconds(args.seconds, stop)
    manager.RegisterAdvertisement(advertisement.path, {}, reply_handler=registered, error_handler=error)
    try:
        loop.run()
    finally:
        if advertisement.registered:
            manager.UnregisterAdvertisement(advertisement.path)
        advertisement.remove_from_connection()
        print("STOPPED temporary advertisement removed; existing bonds untouched", flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
