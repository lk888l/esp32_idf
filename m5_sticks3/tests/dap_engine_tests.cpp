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
static bool enabled=false, cancelled=false, jtag=false;
static unsigned tdi=1, tdo_reads=0, cancel_after=0;
static std::deque<unsigned> tdo_bits;
struct JtagEdge { unsigned tms, tdi; };
static std::vector<JtagEdge> edges;
static unsigned pin=0, timestamp=0;
extern "C" uint32_t fake_timestamp() { return ++timestamp; }
extern "C" void fake_clock(unsigned value) { if(value&&enabled) { driven.push_back(pin); if(jtag) edges.push_back({pin,tdi}); } }
extern "C" void fake_output(unsigned value) { pin=value; }
extern "C" void fake_enable(unsigned value) { enabled=value; }
extern "C" uint32_t fake_input() { assert(!bits.empty()); auto bit=bits.front(); bits.pop_front(); return bit; }
extern "C" uint32_t probe_set_clock(uint32_t) { return 1; }
extern "C" void probe_port_off() { enabled=jtag=false; }
extern "C" void probe_port_swd() { enabled=true; jtag=false; }
extern "C" void probe_port_jtag() { enabled=jtag=true; }
extern "C" uint32_t fake_tdi() { return tdi; }
extern "C" void fake_tdi_output(unsigned value) { tdi=value; }
extern "C" uint32_t fake_tdo() {
    ++tdo_reads;
    if(tdo_bits.empty()) return 0;
    auto bit=tdo_bits.front(); tdo_bits.pop_front(); return bit;
}
extern "C" uint8_t probe_serial(char* p) { std::strcpy(p,"0123456789AB"); return 13; }
extern "C" uint8_t probe_reset() { return 1; }
extern "C" int probe_cancelled() { return cancelled || (cancel_after && tdo_reads>=cancel_after); }
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
void jtag_ack(unsigned code) {
    // JTAG-DP wire ACK is 010=OK / 001=WAIT (SWD uses the reverse).
    tdo_bits.push_back((code>>1)&1); tdo_bits.push_back(code&1); tdo_bits.push_back((code>>2)&1);
}
void jtag_data(uint32_t data, unsigned count=32) {
    for(unsigned i=0;i<count;++i) tdo_bits.push_back((data>>(i%32))&1);
}
void jtag_read(uint32_t data) { jtag_ack(1); jtag_data(data); }
void test_jtag() {
    DAP_Setup(); assert(!enabled); assert(run({2,2}).data[1]==2&&jtag);
    assert(run({0x16,0}).data[1]==0xff); // No configured chain yet.
    // Variable-length sequences: LSB-first TDI/TDO, per-sequence byte padding,
    // constant TMS, and the special count=0 encoding for 64 clocks.
    for(unsigned count:{1U,7U,8U,9U,31U,32U,63U,64U}) {
        uint8_t request[64]{0x14,1,uint8_t(0xc0|(count&63))};
        unsigned bytes=(count+7)/8;
        for(unsigned i=0;i<bytes;++i) request[3+i]=0xa5;
        for(unsigned i=0;i<count;++i) tdo_bits.push_back((0x96>>(i%8))&1);
        uint8_t response[64]{}; edges.clear();
        assert(valid_request(request,3+bytes));
        assert((DAP_ProcessCommand(request,response)&0xffff)==2+bytes);
        assert(response[0]==0x14&&response[1]==0&&edges.size()==count&&tdo_bits.empty());
        for(unsigned i=0;i<count;++i) {
            assert(edges[i].tms==1&&edges[i].tdi==((0xa5>>(i%8))&1));
            assert(((response[2+i/8]>>(i%8))&1)==((0x96>>(i%8))&1));
        }
        if(count%8) assert((response[1+bytes]>>(count%8))==0);
    }
    jtag_data(5,3); jtag_data(0,8); jtag_data(0x101,9);
    edges.clear(); auto out=run({0x14,3,0x83,5,0x48,0xa5,0x89,0xff,1});
    assert(out.size==5&&out.data[2]==5&&out.data[3]==1&&out.data[4]==1);
    assert(edges.size()==20&&edges[2].tms==0&&edges[3].tms==1&&edges[11].tms==0);
    assert(tdo_bits.empty());
    // Three-device IR chain: selected device 1 gets IDCODE, neighbours BYPASS.
    assert(run({0x15,3,5,4,6}).data[1]==0);
    assert(DAP_Data.jtag_dev.ir_before[1]==5&&DAP_Data.jtag_dev.ir_after[1]==6);
    edges.clear(); jtag_data(0x4ba00477); out=run({0x16,1});
    assert(out.size==6&&out.data[1]==0&&read32(out.data+2)==0x4ba00477&&tdo_bits.empty());
    const unsigned ir=0x1fU | (0x0eU << 5) | (0x3fU << 9); // 5 BYPASS ones, IDCODE 1110, 6 BYPASS ones.
    assert(edges.size()>=21);
    for(unsigned i=0;i<15;++i) {
        assert(edges[4+i].tdi==((ir>>i)&1));
        assert(edges[4+i].tms==unsigned(i==14));
    }
    assert(edges[19].tms==1&&edges[20].tms==0); // Update-IR -> Idle.
    auto clocks=edges.size(); out=run({0x16,3});
    assert(out.size==2&&out.data[1]==0xff&&edges.size()==clocks);
    // CoreSight DP/AP pipelining, retry ACK conversion, and block reads.
    run({0x15,1,4});
    jtag_read(0xdeadbeef); jtag_read(0x80000001);
    out=run({5,0,1,2}); assert(out.size==7&&out.data[1]==1&&out.data[2]==1);
    assert(read32(out.data+3)==0x80000001&&tdo_bits.empty());
    jtag_ack(2); jtag_read(0); jtag_read(0xa5a5a5a5); jtag_read(0xffffffff);
    out=run({5,0,2,3,3}); assert(out.size==11&&out.data[1]==2&&out.data[2]==1);
    assert(read32(out.data+3)==0xa5a5a5a5&&read32(out.data+7)==0xffffffff&&tdo_bits.empty());
    jtag_read(0); jtag_read(0x11223344); jtag_read(0x87654321);
    out=run({6,0,2,0,2}); assert(out.size==12&&out.data[1]==2&&out.data[3]==1);
    assert(read32(out.data+4)==0x11223344&&read32(out.data+8)==0x87654321&&tdo_bits.empty());
    jtag_ack(1); jtag_read(0); out=run({5,0,1,0,0xff,0xff,0xff,0xff});
    assert(out.data[1]==1&&out.data[2]==1&&tdo_bits.empty());
    jtag_ack(4); out=run({5,0,1,2}); assert(out.data[1]==0&&out.data[2]==4&&tdo_bits.empty());
    // Cancellation interrupts an otherwise 65535-retry WAIT operation.
    run({4,0,0xff,0xff,0xff,0xff}); tdo_reads=0; cancel_after=3; jtag_ack(2);
    out=run({5,0,1,2}); assert(out.data[1]==0&&out.data[2]==8&&tdo_reads==3);
    cancel_after=0; cancelled=true; out=run({6,0,2,0,2});
    assert(out.data[1]==0&&out.data[3]==8); cancelled=false;
    assert(run({8,1,0,0,0,0}).data[1]==0xff); // ABORT with invalid TAP index.
    assert(run({8,0,0,0,0,0}).data[1]==0);
    assert(run({0x15,0}).data[1]==0&&DAP_Data.jtag_dev.count==0);
    assert(run({0x16,0}).data[1]==0xff);
    run({2,1}); assert(enabled&&!jtag); run({2,2}); assert(jtag);
    run({3}); assert(!enabled&&!jtag);
    DAP_Setup(); assert(DAP_Data.jtag_dev.count==0);
}
int main() {
    DAP_Setup(); assert(!enabled); auto out=run({0,0xf0});
    assert(out.data[0]==0&&out.data[1]==2&&(out.data[2]&3)==3&&!(out.data[2]&0x1c));
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
    test_jtag();
    // Execute every accepted random packet against padded input with SWD/JTAG
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
        if(request[0]==5||request[0]==6) {
            DAP_Data.debug_port=(i&1)?2:1;
            if(DAP_Data.debug_port==2) run({0x15,8,32,32,32,32,32,32,32,32});
        }
        uint8_t response[64]{};
        assert((DAP_ProcessCommand(request,response)&0xffff)<=64);
    }
    cancelled=false;
    DAP_Setup(); run({2,1});
    for(unsigned i=0;i<8;++i) bits.push_back((0xa5>>i)&1);
    out=run({0x1d,1,0x88}); assert(out.size==3&&out.data[1]==0&&out.data[2]==0xa5&&bits.empty());
}
