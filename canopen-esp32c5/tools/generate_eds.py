#!/usr/bin/env python3
"""Generate the EDS matching canopen::StandardProfile.

The generated file is intentionally deterministic.  Keep the small set of
product objects below in sync with StandardProfile::register_standard_objects;
the repetitive PDO records are expanded here to make the resulting EDS usable
by tools that do not implement CompactSubObj.
"""

from __future__ import annotations

import argparse
from pathlib import Path


DT_UNSIGNED8 = 0x0005
DT_UNSIGNED16 = 0x0006
DT_UNSIGNED32 = 0x0007
DT_VISIBLE_STRING = 0x0009


class Eds:
    def __init__(self) -> None:
        self.lines: list[str] = []

    def section(self, name: str, **values: object) -> None:
        self.lines.append(f"[{name}]")
        self.lines.extend(f"{key}={value}" for key, value in values.items())
        self.lines.append("")

    def scalar(
        self,
        index: int,
        name: str,
        data_type: int,
        access: str,
        default: object,
        *,
        pdo: bool = False,
    ) -> None:
        self.section(
            f"{index:04X}",
            ParameterName=name,
            ObjectType="0x7",
            DataType=f"0x{data_type:04X}",
            AccessType=access,
            DefaultValue=default,
            PDOMapping=int(pdo),
        )

    def record(self, index: int, name: str, sub_number: int) -> None:
        self.section(
            f"{index:04X}",
            ParameterName=name,
            ObjectType="0x9",
            SubNumber=sub_number,
        )

    def sub(
        self,
        index: int,
        subindex: int,
        name: str,
        data_type: int,
        access: str,
        default: object,
        *,
        pdo: bool = False,
    ) -> None:
        self.section(
            f"{index:04X}sub{subindex:X}",
            ParameterName=name,
            ObjectType="0x7",
            DataType=f"0x{data_type:04X}",
            AccessType=access,
            DefaultValue=default,
            PDOMapping=int(pdo),
        )

    def render(self) -> str:
        return "\n".join(self.lines).rstrip() + "\n"


def add_identity(eds: Eds) -> None:
    eds.record(0x1018, "Identity object", 5)
    eds.sub(0x1018, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 4)
    eds.sub(0x1018, 1, "Vendor-ID", DT_UNSIGNED32, "ro", "0x00000000")
    eds.sub(0x1018, 2, "Product code", DT_UNSIGNED32, "ro", "0xC5000001")
    eds.sub(0x1018, 3, "Revision number", DT_UNSIGNED32, "ro", "0x00010000")
    eds.sub(0x1018, 4, "Serial number", DT_UNSIGNED32, "ro", "0x00000001")


def add_heartbeat_consumers(eds: Eds) -> None:
    eds.record(0x1016, "Consumer heartbeat time", 5)
    eds.sub(0x1016, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 4)
    for subindex in range(1, 5):
        eds.sub(
            0x1016,
            subindex,
            f"Consumer heartbeat {subindex}",
            DT_UNSIGNED32,
            "rw",
            "0x00000000",
        )


def add_store_parameters(eds: Eds) -> None:
    eds.record(0x1010, "Store parameters", 2)
    eds.sub(0x1010, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 1)
    eds.sub(0x1010, 1, "Save all parameters", DT_UNSIGNED32, "rw", "0x00000001")


def add_sdo_server(eds: Eds) -> None:
    eds.record(0x1200, "SDO server parameter", 3)
    eds.sub(0x1200, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 2)
    eds.sub(0x1200, 1, "COB-ID client to server", DT_UNSIGNED32, "ro", "$NODEID+0x600")
    eds.sub(0x1200, 2, "COB-ID server to client", DT_UNSIGNED32, "ro", "$NODEID+0x580")


def add_pdo_mapping(eds: Eds, index: int, name: str) -> None:
    eds.record(index, name, 17)
    eds.sub(index, 0, "Number of mapped objects", DT_UNSIGNED8, "rw", 0)
    for subindex in range(1, 17):
        eds.sub(
            index,
            subindex,
            f"Mapped object {subindex}",
            DT_UNSIGNED32,
            "rw",
            "0x00000000",
        )


def add_rpdo(eds: Eds, channel: int) -> None:
    comm_index = 0x1400 + channel
    map_index = 0x1600 + channel
    cob_base = (0x200, 0x300, 0x400, 0x500)[channel]
    eds.record(comm_index, f"RPDO{channel + 1} communication parameter", 3)
    eds.sub(comm_index, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 2)
    eds.sub(
        comm_index,
        1,
        "COB-ID used by RPDO",
        DT_UNSIGNED32,
        "rw",
        f"$NODEID+0x{cob_base:X}",
    )
    eds.sub(comm_index, 2, "Transmission type", DT_UNSIGNED8, "rw", 255)
    add_pdo_mapping(eds, map_index, f"RPDO{channel + 1} mapping parameter")


def add_tpdo(eds: Eds, channel: int) -> None:
    comm_index = 0x1800 + channel
    map_index = 0x1A00 + channel
    cob_base = (0x40000180, 0x40000280, 0x40000380, 0x40000480)[channel]
    eds.record(comm_index, f"TPDO{channel + 1} communication parameter", 6)
    eds.sub(comm_index, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 5)
    eds.sub(
        comm_index,
        1,
        "COB-ID used by TPDO",
        DT_UNSIGNED32,
        "rw",
        f"$NODEID+0x{cob_base:X}",
    )
    eds.sub(comm_index, 2, "Transmission type", DT_UNSIGNED8, "rw", 255)
    eds.sub(comm_index, 3, "Inhibit time in 100 us", DT_UNSIGNED16, "rw", 0)
    eds.sub(comm_index, 4, "Compatibility entry", DT_UNSIGNED8, "rw", 0)
    eds.sub(comm_index, 5, "Event timer in ms", DT_UNSIGNED16, "rw", 0)
    add_pdo_mapping(eds, map_index, f"TPDO{channel + 1} mapping parameter")


def add_manufacturer_record(eds: Eds) -> None:
    eds.record(0x2000, "Framework diagnostics", 5)
    eds.sub(0x2000, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 4)
    eds.sub(0x2000, 1, "Uptime in ms", DT_UNSIGNED32, "ro", 0, pdo=True)
    eds.sub(0x2000, 2, "Received PDO count", DT_UNSIGNED32, "ro", 0, pdo=True)
    eds.sub(0x2000, 3, "Transmitted PDO count", DT_UNSIGNED32, "ro", 0, pdo=True)
    eds.sub(0x2000, 4, "Application value", DT_UNSIGNED32, "rw", 0, pdo=True)


def add_node_configuration(eds: Eds) -> None:
    eds.record(0x2001, "Node configuration", 2)
    eds.sub(0x2001, 0, "Highest sub-index supported", DT_UNSIGNED8, "ro", 1)
    eds.sub(0x2001, 1, "Node-ID after restart", DT_UNSIGNED8, "rw", "$NODEID")


def generate() -> str:
    eds = Eds()
    eds.lines.extend(
        [
            "; Generated by tools/generate_eds.py. Do not edit manually.",
            "; CANopen control services use classic CAN; PDO payloads above 8 bytes use FD+BRS.",
            "",
        ]
    )
    eds.section(
        "FileInfo",
        FileName="esp32c5_canopen_fd.eds",
        FileVersion=1,
        FileRevision=2,
        EDSVersion="4.0",
        Description="ESP32-C5 CANopen with CAN-FD PDO extension",
        CreationTime="06:00AM",
        CreationDate="08-05-2026",
        CreatedBy="CANopen ESP32-C5 project",
        ModificationTime="06:00AM",
        ModificationDate="08-05-2026",
        ModifiedBy="CANopen ESP32-C5 project",
    )
    eds.section(
        "DeviceInfo",
        VendorName="Project vendor",
        VendorNumber="0x00000000",
        ProductName="ESP32-C5 CANopen-FD Node",
        ProductNumber="0xC5000001",
        RevisionNumber="0x00010000",
        OrderCode="ESP32C5-CANOPEN-FD",
        BaudRate_10=0,
        BaudRate_20=0,
        BaudRate_50=0,
        BaudRate_125=0,
        BaudRate_250=0,
        BaudRate_500=0,
        BaudRate_800=0,
        BaudRate_1000=1,
        SimpleBootUpMaster=0,
        SimpleBootUpSlave=1,
        Granularity=8,
        DynamicChannelsSupported=0,
        GroupMessaging=0,
        NrOfRXPDO=4,
        NrOfTXPDO=4,
        LSS_Supported=0,
    )
    eds.section(
        "DeviceComissioning",
        NodeID="0x21",
        NodeName="ESP32-C5 CANopen-FD Node",
        Baudrate=1000,
    )
    eds.section(
        "CANopenFD",
        Profile="Classic control plane with CAN-FD+BRS PDO extension",
        NominalBitRate=1000000,
        NominalSamplePointPermill=800,
        NominalSJW=5,
        DataBitRate=5000000,
        DataSamplePointPermill=750,
        DataSJW=3,
        MaxPdoPayload=64,
    )

    mandatory = [0x1000, 0x1001, 0x1018]
    optional = [
        0x1005,
        0x1006,
        0x1008,
        0x1009,
        0x100A,
        0x1010,
        0x1013,
        0x1016,
        0x1017,
        0x1200,
        0x1400,
        0x1401,
        0x1402,
        0x1403,
        0x1600,
        0x1601,
        0x1602,
        0x1603,
        0x1800,
        0x1801,
        0x1802,
        0x1803,
        0x1A00,
        0x1A01,
        0x1A02,
        0x1A03,
    ]
    eds.section(
        "MandatoryObjects",
        SupportedObjects=len(mandatory),
        **{str(i): f"0x{index:04X}" for i, index in enumerate(mandatory, 1)},
    )
    eds.section(
        "OptionalObjects",
        SupportedObjects=len(optional),
        **{str(i): f"0x{index:04X}" for i, index in enumerate(optional, 1)},
    )
    eds.section("ManufacturerObjects", SupportedObjects=2, **{"1": "0x2000", "2": "0x2001"})

    eds.scalar(0x1000, "Device type", DT_UNSIGNED32, "ro", "0x00000000")
    eds.scalar(0x1001, "Error register", DT_UNSIGNED8, "ro", 0, pdo=True)
    eds.scalar(0x1005, "COB-ID SYNC message", DT_UNSIGNED32, "ro", "0x00000080")
    eds.scalar(0x1006, "Communication cycle period", DT_UNSIGNED32, "rw", 0)
    eds.scalar(
        0x1008,
        "Manufacturer device name",
        DT_VISIBLE_STRING,
        "ro",
        "ESP32-C5 CANopen-FD Node",
    )
    eds.scalar(0x1009, "Manufacturer hardware version", DT_VISIBLE_STRING, "ro", "ESP32-C5")
    eds.scalar(0x100A, "Manufacturer software version", DT_VISIBLE_STRING, "ro", "0.2.0")
    add_store_parameters(eds)
    eds.scalar(0x1013, "High resolution timestamp", DT_UNSIGNED32, "ro", 0, pdo=True)
    add_heartbeat_consumers(eds)
    eds.scalar(0x1017, "Producer heartbeat time", DT_UNSIGNED16, "rw", 100)
    add_identity(eds)
    add_sdo_server(eds)
    for channel in range(4):
        add_rpdo(eds, channel)
    for channel in range(4):
        add_tpdo(eds, channel)
    add_manufacturer_record(eds)
    add_node_configuration(eds)
    return eds.render()


def main() -> None:
    default_output = Path(__file__).resolve().parents[1] / "eds" / "esp32c5_canopen_fd.eds"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=default_output)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
