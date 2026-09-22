#include "dap_protocol.hpp"
#include <cassert>
#include <cstring>
#include <deque>
#include <vector>
#include <random>
extern "C" {
#include "DAP_config.h"
#include "DAP.h"
}
using namespace debug_probe;
static std::deque<unsigned> bits;
static std::vector<unsigned> driven;
static bool enabled=false, cancelled=false;
static unsigned pin=0, timestamp=0;
extern "C" uint32_t fake_timestamp() { return ++timestamp; }
extern "C" void fake_clock(unsigned value) { if(value&&enabled) driven.push_back(pin); }
extern "C" void fake_output(unsigned value) { pin=value; }
extern "C" void fake_enable(unsigned value) { enabled=value; }
extern "C" uint32_t fake_input() { assert(!bits.empty()); auto bit=bits.front(); bits.pop_front(); return bit; }
extern "C" uint32_t probe_set_clock(uint32_t) { return 1; }
extern "C" void probe_port_off() { enabled=false; }
extern "C" void probe_port_swd() { enabled=true; }
extern "C" uint8_t probe_serial(char* p) { std::strcpy(p,"0123456789AB"); return 13; }
extern "C" uint8_t probe_reset() { return 1; }
extern "C" int probe_cancelled() { return cancelled; }
extern "C" void probe_host_status(unsigned,unsigned) {}
void ack(unsigned code) { for(unsigned i=0;i<3;++i) bits.push_back((code>>i)&1); }
void read(unsigned data, bool parity_error=false) {
    ack(1); unsigned parity=0;
    for(unsigned i=0;i<32;++i) { auto bit=(data>>i)&1; bits.push_back(bit); parity^=bit; }
    bits.push_back(parity^parity_error);
}
Packet run(std::initializer_list<uint8_t> data) {
    assert(valid_request(data.begin(),data.size()));
    uint8_t input[64]{}; std::memcpy(input,data.begin(),data.size());
    Packet output{}; output.size=DAP_ProcessCommand(input,output.data)&0xffff;
    assert(output.size<=64); return output;
}
int main() {
    DAP_Setup(); assert(!enabled); auto out=run({0,0xf0});
    assert(out.data[0]==0&&out.data[1]==2&&(out.data[2]&1)&&!(out.data[2]&0x1e));
    assert(run({0,0xff}).data[2]==64); assert(run({0,0xfe}).data[2]==1);
    assert(run({2,1}).data[1]==1&&enabled);
    // DP IDCODE read, full bit-level request and response parity.
    read(0x2ba01477); out=run({5,0,1,2}); assert(out.size==7&&out.data[1]==1&&out.data[2]==1);
    assert(read32(out.data+3)==0x2ba01477);
    const std::vector<unsigned> expected{1,0,1,0,0,1,0,1};
    assert(std::equal(expected.begin(),expected.end(),driven.begin()));
    // WAIT is retried, FAULT and parity errors are reported to the host.
    ack(2); read(0xabcdef80); out=run({5,0,1,2}); assert(out.data[2]==1&&read32(out.data+3)==0xabcdef80);
    ack(4); out=run({5,0,1,2}); assert(out.data[1]==0&&out.data[2]==4);
    read(1,true); out=run({5,0,1,2}); assert(out.data[2]==8);
    // AP posted reads flush the final result through DP_RDBUFF.
    read(0xdeadbeef); read(0x11111111); read(0x22222222);
    out=run({5,0,2,3,3}); assert(out.size==11&&out.data[1]==2);
    assert(read32(out.data+3)==0x11111111&&read32(out.data+7)==0x22222222);
    // Write data with bit 31 set, followed by reference write completion check.
    ack(1); read(0); out=run({5,0,1,0,0xff,0xff,0xff,0xff}); assert(out.data[2]==1);
    read(0x80000001); read(0xffffffff); out=run({6,0,2,0,2});
    assert(out.size==12&&out.data[1]==2&&out.data[3]==1&&read32(out.data+8)==0xffffffff);
    cancelled=true; out=run({5,0,1,2}); assert(out.data[2]==8); cancelled=false;
    run({3}); assert(!enabled); assert(bits.empty());
    // Execute every accepted random packet against padded input with SWD
    // cancelled: sanitizers catch mistakes in worst-case response accounting.
    std::mt19937 rng(0x444150);
    for(unsigned i=0;i<20000;++i) {
        uint8_t request[64]{};
        for(auto& byte:request) byte=rng();
        const size_t length=1+rng()%64;
        // Sequences would need an arbitrary GPIO waveform; covered separately.
        if(request[0]==0x1d||request[0]==0x12||request[0]==0x10) continue;
        if(!valid_request(request,length)||request[0]==7) continue;
        DAP_Setup(); cancelled=true;
        if(request[0]==5||request[0]==6) DAP_Data.debug_port=1;
        uint8_t response[64]{};
        assert((DAP_ProcessCommand(request,response)&0xffff)<=64);
    }
    cancelled=false;
    DAP_Setup(); run({2,1});
    for(unsigned i=0;i<8;++i) bits.push_back((0xa5>>i)&1);
    out=run({0x1d,1,0x88}); assert(out.size==3&&out.data[1]==0&&out.data[2]==0xa5&&bits.empty());
}
