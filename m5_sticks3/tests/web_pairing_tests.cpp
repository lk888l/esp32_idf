#include "../components/connectivity/include/web_pairing.hpp"
#include "platform.hpp"
#include <cassert>
int main()
{
    using namespace connectivity;
    assert(!local_web_pairing_code());
    auto code = open_web_pairing();
    assert(code >= 100000 && code <= 999999);
    assert(!consume_web_pairing(code + 1));
    assert(consume_web_pairing(code));
    assert(!consume_web_pairing(code));
    code = open_web_pairing();
    fake::now += 120'000'000;
    assert(!local_web_pairing_code());
    assert(!consume_web_pairing(code));
    code = open_web_pairing();
    for (int i = 0; i < 5; ++i)
        assert(!consume_web_pairing(code + 1));
    assert(!consume_web_pairing(code));
    code = open_web_pairing();
    auto generation = web_session_generation();
    revoke_web_sessions();
    assert(!consume_web_pairing(code));
    assert(web_session_generation() == generation + 1);
}
