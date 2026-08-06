#pragma once

#include "wireless/protocol.hpp"

#include <cstdint>

namespace wireless_esp32 {

enum class LinkKind : uint8_t { tcp = 0, ble = 1 };
inline constexpr uint8_t kBroadcastPeer = 0xFF;

struct InboundPacket {
    LinkKind link = LinkKind::tcp;
    uint8_t peer = 0;
    wireless::Packet packet{};
};

class PacketSink {
public:
    virtual ~PacketSink() = default;
    virtual bool submit(const InboundPacket& packet) = 0;
};

} // namespace wireless_esp32
