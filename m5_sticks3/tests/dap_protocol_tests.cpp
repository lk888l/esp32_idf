#include "dap_protocol.hpp"
#include "dap_session.hpp"
#include <cassert>
#include <initializer_list>
#include <random>
using namespace debug_probe;
bool valid(std::initializer_list<uint8_t> p) { return valid_request(p.begin(), p.size()); }
int main() {
    assert(!valid({})); assert(!valid({0})); assert(valid({0,0xff}));
    assert(valid({5,0,1,2})); assert(!valid({5,0,2,2}));
    assert(!valid({5,0,1,0,1,2,3})); assert(valid({5,0,1,0,1,2,3,0xff}));
    assert(valid({5,0,1,0x12,1,2,3,4})); assert(!valid({5,0,1,0x12,1,2,3}));
    assert(!valid({5,0,1,0x62}));
    uint8_t p[64]{5,0,15};
    for (int i=0;i<15;++i) p[3+i]=2;
    assert(valid_request(p,18)); p[2]=16; p[18]=2; assert(!valid_request(p,19));
    assert(valid({6,0,15,0,2})); assert(!valid({6,0,16,0,2}));
    assert(!valid({6,0,0xff,0xff,2})); assert(!valid({6,0,1,0,0}));
    assert(valid({6,0,1,0,0,0xff,0xff,0xff,0xff}));
    assert(valid({0x1d,2,0x88,8,0xa5})); assert(!valid({0x1d,2,0x88,8}));
    assert(!valid({0x1d,8,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80}));
    assert(!valid({0x12,0})); assert(!valid({0x7f,0xff})); assert(!valid({0x80}));
    uint8_t h[8]{}; uint16_t n=0; tcp_header(h,64,1); assert(tcp_length(h,n)&&n==64);
    h[6]=2; assert(!tcp_length(h,n)); h[6]=1; h[7]=1; assert(!tcp_length(h,n));
    h[7]=0; h[4]=65; assert(!tcp_length(h,n));
    BleAssembler a;
    uint8_t first[]{1,0,0,3,0x05,0}; uint8_t last[]{1,0,2,3,0};
    assert(a.append(first,sizeof(first))==BleAssembler::Result::partial);
    last[2]=1; assert(a.append(last,sizeof(last))==BleAssembler::Result::invalid);
    last[2]=2; assert(a.append(last,sizeof(last))==BleAssembler::Result::invalid);
    assert(a.append(first,sizeof(first))==BleAssembler::Result::partial);
    assert(a.append(last,sizeof(last))==BleAssembler::Result::complete);
    assert(a.packet.size==3&&a.packet.data[0]==5&&a.sequence==1);
    a.reset(); uint8_t large[]{1,0,0,65,0}; assert(a.append(large,sizeof(large))==BleAssembler::Result::invalid);
    BleSession mailbox;
    uint8_t command[]{1,0,0,1,3};
    assert(mailbox.write(7,command,sizeof(command),42));
    assert(!mailbox.write(7,command,sizeof(command),42)); // backpressure
    assert(!mailbox.write(8,command,sizeof(command),42)); // ownership
    Packet request{}, reply{}; reply.size=2; reply.data[0]=3;
    BleSession::Token old_token, new_token;
    assert(!mailbox.take(41,request,old_token));
    assert(mailbox.take(42,request,old_token));
    assert(mailbox.disconnect(7));
    // Same connection handle and sequence reused by a new host.
    assert(mailbox.write(7,command,sizeof(command),42));
    assert(mailbox.take(42,request,new_token));
    assert(!mailbox.complete(old_token,reply));
    assert(mailbox.complete(new_token,reply));
    uint8_t response[69]{};
    assert(mailbox.read(8,response,sizeof(response))==0);
    assert(mailbox.read(7,response,sizeof(response))==7&&response[2]==1&&response[5]==3);
    mailbox.reset(); assert(mailbox.read(7,response,sizeof(response))==0);
    // Adversarial parser smoke test under ASan/UBSan, including truncated packets.
    std::mt19937 rng(0x535744);
    for (unsigned i=0;i<100000;++i) {
        for (auto& x:p) x=rng();
        const size_t size=rng()%65;
        (void)valid_request(p,size); (void)a.append(p,size);
    }
}
