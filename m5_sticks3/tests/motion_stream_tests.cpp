#include "motion_stream.hpp"
#include <cassert>
int main()
{
    auto &stream = model::MotionStream::instance();
    assert(!stream.requested());
    stream.request(true);
    assert(stream.requested());
    uint32_t a = 0, b = 0, lost = 0;
    model::StreamSample out[5];
    assert(stream.read(a, out, 5, lost) == 0);
    for (unsigned i = 1; i <= 140; ++i)
        stream.publish({i * 10000ULL, 0, 1, 2, 3, true});
    assert(stream.read(a, out, 5, lost) == 5 && lost == 12 && a == 17);
    assert(out[0].sequence == 13 && out[0].time_us == 130000);
    uint32_t lost_b = 0;
    assert(stream.read(b, out, 5, lost_b) == 5 && b == 17);
    while (stream.read(a, out, 5, lost))
    {
    };
    assert(a == 140 && stream.latest() == 140);
    stream.request(false);
    assert(!stream.requested());
    stream.publish({1410000, 0, 0, 0, 0, false});
    assert(stream.read(a, out, 5, lost) == 1 && !out[0].valid);
}
