# Wireless CAN/CAN-FD gateway

This document defines the product boundary, deployment workflow, and HX v1 wire protocol for the ESP32-C5 wireless gateway. The implementation lives in `wireless_protocol`, `wireless_esp32`, and `EspCanGateway`; it is part of the main firmware rather than a standalone demo.

## Data path

```text
secure BLE GATT --+
                  +--> WirelessModule ingress --> EspCanGateway ingress
authenticated TCP+                                  |
                                                     +--> physical TWAI-FD bus
                                                     +--> local CANopen profile

physical TWAI-FD --+
local CANopen TX --+--> EspCanGateway monitor --> WirelessModule --> TCP/BLE
wireless ingress --+
```

The high-priority `canopen` task remains the sole owner of CANopen state. Wireless tasks cannot modify the object dictionary directly. A wireless CAN frame enters a bounded queue, then the CANopen task handles it in order and attempts to forward it to the physical bus. Physical RX, local TX, and wireless ingress frames enter the monitor queue with an origin and a microsecond timestamp.

| Task | Default priority | Responsibility |
|---|---:|---|
| `canopen` | 12 | TWAI RX, wireless ingress, CANopen state |
| `wireless_gateway` | 9 | HX routing and CAN monitor fan-out |
| `wireless_tcp` | 8 | UDP discovery, TCP authentication, socket I/O |
| NimBLE host | ESP-IDF setting | BLE GAP/GATT |

All real-time queues, stream decoders, CAN frames, BLE packets, and per-client TX rings have fixed capacity. A full queue is counted and rejected. A persistently slow TCP client is disconnected without blocking the CANopen task.

## Wireless modes

- SoftAP is always available as `HexMotor-C5-XXXXXX`; the suffix is derived from the STA MAC.
- SoftAP uses a build-time WPA2/WPA3 password. The password is never logged or returned by the protocol.
- The device starts in AP mode when no station is configured.
- Secure BLE provisioning stores station credentials and changes the mode to APSTA.
- Station credentials are stored in NVS namespace `wireless` as `sta_ssid` and `sta_pass`.
- Station reconnect uses a 1, 2, 4, 8, 16, then 30 second capped backoff.
- SoftAP and CAN services remain active while STA reconnects.
- Wi-Fi power save is disabled to reduce interactive debugging latency.
- The default tunnel is TCP 3333; discovery is UDP 3334.

`WifiState` values are 0 stopped, 1 ap_only, 2 connecting, and 3 connected.

## Fail-closed build

Wireless is disabled by default and the repository contains no usable credentials. Create a private overlay:

```sh
cp sdkconfig.secrets.example sdkconfig.secrets
chmod 600 sdkconfig.secrets
```

Set all required values:

```text
CONFIG_WIRELESS_GATEWAY_ENABLED=y
CONFIG_WIRELESS_AP_PASSWORD="<8..63 characters>"
CONFIG_WIRELESS_BLE_PASSKEY=<100000..999999>
CONFIG_WIRELESS_TCP_CONTROL_KEY_HEX="<64 non-zero hex digits>"
```

Use independent values. A 256-bit control key can be generated with `openssl rand -hex 32`. Both `sdkconfig.secrets` and generated `sdkconfig.local` are ignored by Git. The wrapper loads the overlay automatically:

```sh
./tools/idf.sh build
./tools/idf.sh -p /dev/ttyACM0 flash monitor
```

The firmware uses ESP-IDF's official 1500 KiB single-app-large partition table and keeps the build compatible with 2 MiB flash while preserving NVS and PHY. The connected board reports a 4 MiB device, but the current partition table intentionally does not consume the extra space. OTA requires a separately designed dual-OTA layout and an explicit 4 MiB product configuration.

The wireless module fails closed if any secret is missing, malformed, out of range, or if the TCP key is all zero. Logs include SSID, ports, state, and counters, but never the AP password, BLE passkey, or TCP key.

## Security model

BLE:

- LE Secure Connections, MITM, and bonding are required.
- RX requires encryption, authentication, and a 16-byte encryption key.
- TX notifications are sent only to an encrypted, authenticated, subscribed connection.
- Station credentials are accepted only through secure BLE; TCP receives `forbidden`.
- Repeat pairing removes the stale peer bond and retries.

TCP:

- Mutual HMAC-SHA256 challenge-response must complete within 10 seconds.
- Client and device nonces are 16 bytes each; device randomness comes from the hardware RNG.
- The client verifies the device proof before the device verifies the client proof.
- Proof comparison on the device is constant-time.
- Unauthenticated clients cannot read status, monitor CAN, or inject CAN.
- Each client uses `TCP_NODELAY`, keepalive, a resynchronizing decoder, and an eight-packet fixed TX ring.
- Eight consecutive invalid packets, authentication failure/timeout, or sustained backpressure closes the connection.

HX CRC32 detects corruption; it is not a MAC. TCP payloads are not independently encrypted, so confidentiality relies on the WPA2/WPA3 wireless link. The supported deployment boundary is the device SoftAP or a controlled LAN. Do not expose TCP 3333 to the public internet. Untrusted LAN/WAN deployment requires a new protocol version with TLS 1.3 PSK or certificate identity.

For production, also evaluate Secure Boot, flash encryption, unique per-device keys, and key rotation. Build-time secrets do not protect against an attacker who can read flash.

## BLE GATT

| Item | UUID | Properties |
|---|---|---|
| Service | `41574554-4147-524f-544f-4da55ac5c001` | primary |
| RX | `41574554-4147-524f-544f-4da55ac5c002` | write/write-no-response, encrypted and authenticated |
| TX | `41574554-4147-524f-544f-4da55ac5c003` | notify |

The preferred MTU is 247. The maximum HX packet is 144 bytes, so one ATT value carries one complete packet. Pair, subscribe to TX, then write a complete HX packet to RX. BLE does not repeat the TCP HMAC handshake because the GATT attribute itself requires authenticated encryption.

`wifi_credentials (0x30)` payload:

```text
u8 ssid_length
u8 password_length
u8 ssid[ssid_length]           # 1..32 bytes
u8 password[password_length]   # 0..63 bytes
```

`wifi_result (0x31)` contains one byte: 1 means stored and applied; 0 means failure. Credentials are never echoed.

## UDP discovery

Broadcast the seven ASCII bytes `HXDISC1` to UDP 3334. The reply is exactly 24 bytes:

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 7 | bytes | `HXDISC1` |
| 7 | 1 | u8 | protocol version |
| 8 | 2 | u16 LE | TCP port |
| 10 | 4 | u32 LE | capabilities |
| 14 | 6 | bytes | Wi-Fi STA MAC |
| 20 | 4 | u32 LE | CRC32/IEEE over bytes 0..19 |

Discovery locates a device but does not authenticate it. It returns no SSID or secret.

## HX v1 packet

All multi-byte fields are little-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | ASCII `HX` |
| 2 | 1 | version = 1 |
| 3 | 1 | message type |
| 4 | 2 | flags |
| 6 | 2 | payload length, 0..128 |
| 8 | 4 | sequence |
| 12 | N | payload |
| 12+N | 4 | CRC32/IEEE over header and payload |

Flag bit 0 is response; bit 1 is error. Other bits are reserved.

The TCP decoder handles fragmentation and coalescing across arbitrary `recv()` boundaries. It resynchronizes one byte at a time after invalid magic, version, type, length, or CRC. A BLE RX write must contain exactly one complete packet.

| Value | Name | Direction |
|---:|---|---|
| `0x01` | hello | client to device |
| `0x02` | device_info | device to client |
| `0x03` | auth_request | TCP client to device |
| `0x04` | auth_challenge | device to TCP client |
| `0x05` | auth_response | TCP client to device |
| `0x06` | auth_result | device to TCP client |
| `0x10` | can_frame | bidirectional |
| `0x20` | status_request | client to device |
| `0x21` | status_response | device to client |
| `0x30` | wifi_credentials | secure BLE to device |
| `0x31` | wifi_result | device to secure BLE |
| `0x40` | ping | client to device |
| `0x41` | pong | device to client |
| `0x7F` | error | device to client |

A response keeps the request sequence. Asynchronous CAN monitor packets use the device sequence counter.

## TCP authentication

`auth_request` contains `client_nonce[16]`.

`auth_challenge` contains:

```text
client_nonce[16]
device_nonce[16]
HMAC-SHA256(key, "HX-AUTH-DEVICE-v1" || client_nonce || device_nonce)[32]
```

After checking the device proof, the client sends `auth_response`:

```text
HMAC-SHA256(key, "HX-AUTH-CLIENT-v1" || client_nonce || device_nonce)[32]
```

`auth_result` contains 1 or 0. A failed result also sets the error flag and is fully flushed before the connection closes. Every connection requires fresh nonces.

## CAN/CAN-FD payload

`can_frame (0x10)` payload:

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| 0 | 4 | u32 LE | arbitration ID |
| 4 | 1 | u8 | logical data length |
| 5 | 1 | flags | bit0 extended, bit1 RTR, bit2 FD, bit3 BRS |
| 6 | 1 | u8 | origin: 0 physical, 1 local, 2 wireless |
| 7 | 1 | u8 | bus index, currently 0 |
| 8 | 8 | u64 LE | device timestamp in microseconds |
| 16 | N | bytes | CAN payload |

Rules:

- standard ID is 0..0x7FF; extended ID is 0..0x1FFFFFFF;
- classic payload is 0..8; FD payload is 0..64;
- RTR and FD cannot both be set; BRS requires FD;
- a client sends origin 2, bus 0, timestamp 0;
- physical RX is origin 0, local CANopen TX is origin 1;
- accepted wireless ingress is routed and monitored as origin 2.

The tunnel is asynchronous and has no per-frame CAN ACK message. An origin-2 monitor echo means the frame left wireless ingress and entered the common CAN route. Physical ACK and error health are observed through TWAI status counters.

## Device information and status

`device_info` payload:

```text
u32 capabilities
u32 uptime_ms
u8  canopen_node_id
u8  wifi_state
u8  ble_connected
u8  protocol_version
u32 station_ipv4_raw
u8  wifi_sta_mac[6]
u8  firmware_version_length
u8  firmware_version[firmware_version_length]
```

Capabilities are currently `0x0000003F`.

`status_response` contains these 26 `u32 LE` values in order:

```text
wifi_state, station_ipv4_raw, ap_clients,
can_rx_frames, can_rx_dropped, can_tx_frames, can_tx_failed,
can_bus_errors, can_recoveries,
gateway_ingress_frames, gateway_ingress_dropped,
gateway_forwarded_frames, gateway_monitor_dropped,
wireless_ingress_packets, wireless_ingress_dropped, wireless_protocol_errors,
tcp_authenticated, tcp_rx_packets, tcp_tx_packets, tcp_rx_invalid, tcp_tx_dropped,
ble_connections, ble_rx_packets, ble_tx_packets, ble_rx_invalid, ble_tx_dropped
```

Counters are monotonic 32-bit values since boot and naturally wrap.

An `error` payload is `u8 error_code, u8 offending_type`: 1 invalid packet, 2 forbidden, 3 queue full, 4 unsupported.

## Reference client

`tools/wireless_client.py` uses only the Python 3 standard library. Put the same control key in a protected file containing exactly 64 hexadecimal characters:

```sh
chmod 600 ./control.key
python3 tools/wireless_client.py selftest
python3 tools/wireless_client.py discover
python3 tools/wireless_client.py --host 192.168.4.1 --key-file ./control.key info
python3 tools/wireless_client.py --host 192.168.4.1 --key-file ./control.key status
python3 tools/wireless_client.py --host 192.168.4.1 --key-file ./control.key monitor
python3 tools/wireless_client.py --host 192.168.4.1 --key-file ./control.key \
  send 0x321 "01 02 03 04"
python3 tools/wireless_client.py --host 192.168.4.1 --key-file ./control.key \
  send 0x18FF50E5 "00 01 02 03 04 05 06 07 08 09 0A 0B" --fd --brs
```

`HX_CONTROL_KEY_FILE` may contain the key-file path. If no file is given, the client uses hidden input. There is intentionally no `--key-hex` option because it would expose the secret in shell history and process listings.

## hex-motor-gui integration

Add an `HxWirelessTransport` below the GUI layer instead of accessing sockets from UI code:

1. Discover candidates and use MAC, not DHCP address, as device identity.
2. Connect, authenticate mutually, then send hello and validate version/capabilities.
3. Let one I/O thread own the socket and stream decoder.
4. Convert received `can_frame` packets into the GUI's existing common CAN frame type.
5. Put GUI transmit requests into a bounded MPSC queue consumed by the I/O thread.
6. On disconnect, stop injection, discard expired control frames, and reconnect with backoff.
7. Do not resume control until re-authentication succeeds.
8. Map status counters to a diagnostics view, especially drops, TX failures, and bus errors.
9. Keep BLE provisioning in a separate device-onboarding flow; use TCP for the main CAN data plane.

Treat the Python client as the byte-level golden implementation. A production C++ or Rust transport should add automated tests for fragmentation, coalescing, CRC failure, wrong keys, slow clients, and reconnect.

## Verification checklist

```sh
cmake -S tests -B /tmp/canopen-esp32c5-tests
cmake --build /tmp/canopen-esp32c5-tests -j
ctest --test-dir /tmp/canopen-esp32c5-tests --output-on-failure
python3 tools/wireless_client.py selftest
```

Also verify:

- a build without the secret overlay keeps wireless disabled;
- malformed secrets fail closed;
- the full image passes the 1500 KiB partition check;
- startup logs contain no AP password, BLE passkey, or TCP key;
- SoftAP, BLE advertising, TCP/UDP, and TWAI-FD start together;
- the candleLight smoke test still passes under wireless coexistence;
- a wrong TCP key is rejected and a correct key can read, monitor, and send;
- an unpaired BLE write fails, while secure provisioning succeeds;
- a slow TCP client cannot block CANopen processing.

