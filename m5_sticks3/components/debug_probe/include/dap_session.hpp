#pragma once
#include "dap_protocol.hpp"
namespace debug_probe {
// Fixed-size one-command BLE mailbox. All calls are serialized by the service
// lock. Transaction IDs survive disconnects, so a late result can never become
// the first result of a new peer even if NimBLE reuses its connection handle.
class BleSession {
public:
    struct Token { uint32_t generation=0, transaction=0; uint16_t connection=0xffff, sequence=0; };
    void reset() {
        assembler_ = {}; request_ = {}; response_ = {};
        connection_ = 0xffff; sequence_ = 0;
        pending_ = busy_ = ready_ = false; ++transaction_;
    }
    bool write(uint16_t connection, const uint8_t* data, size_t size, uint32_t generation) {
        if (busy_ || (connection_ != 0xffff && connection_ != connection)) return false;
        const auto result = assembler_.append(data,size);
        if (result == BleAssembler::Result::invalid) return false;
        connection_ = connection;
        if (result == BleAssembler::Result::complete) {
            request_ = assembler_.packet; sequence_ = assembler_.sequence;
            generation_ = generation; ++transaction_;
            ready_ = false; busy_ = pending_ = true; assembler_.reset();
        }
        return true;
    }
    bool take(uint32_t generation, Packet& packet, Token& token) {
        if (!pending_ || generation_ != generation) return false;
        packet=request_; token={generation_,transaction_,connection_,sequence_};
        pending_=false; return true;
    }
    bool complete(const Token& token, const Packet& packet) {
        if (token.transaction!=transaction_ || token.generation!=generation_ ||
            token.connection!=connection_ || token.sequence!=sequence_ || !busy_) return false;
        response_=packet; ready_=true; busy_=false; return true;
    }
    size_t read(uint16_t connection, uint8_t* output, size_t capacity) const {
        if(connection!=connection_ || capacity<kPacketSize+5) return 0;
        write16(output,sequence_); output[2]=ready_?1:0;
        write16(output+3,ready_?response_.size:0);
        if(ready_) std::memcpy(output+5,response_.data,response_.size);
        return 5+(ready_?response_.size:0);
    }
    bool disconnect(uint16_t connection) {
        if(connection!=connection_) return false;
        reset(); return true;
    }
private:
    BleAssembler assembler_;
    Packet request_, response_;
    uint32_t generation_=0, transaction_=0;
    uint16_t connection_=0xffff, sequence_=0;
    bool pending_=false, busy_=false, ready_=false;
};
}
