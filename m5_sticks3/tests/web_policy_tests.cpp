#include "web_policy.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
using namespace connectivity::web;
int main()
{
    auto r = std::make_unique<ReplayWindow>();
    const char *result = nullptr;
    assert(r->check(1, "one", result) == ReplayWindow::Check::fresh);
    r->save(1, "one", "accepted");
    assert(r->check(1, "one", result) == ReplayWindow::Check::replay);
    assert(!std::strcmp(result, "accepted"));
    assert(r->check(1, "changed", result) == ReplayWindow::Check::conflict);
    for (uint32_t i = 2; i <= 33; ++i)
        r->save(i, "next", "ok");
    assert(r->check(1, "one", result) == ReplayWindow::Check::expired);
    assert(r->check(0, "", result) == ReplayWindow::Check::expired);
    assert(r->check(34, "", result) == ReplayWindow::Check::fresh);
    DropQueue<int, 4> q;
    for (int i = 1; i <= 4; ++i)
        assert(!q.push(i));
    assert(q.push(5));
    int value;
    for (int i = 2; i <= 5; ++i)
    {
        assert(q.pop(value));
        assert(value == i);
    }
    assert(!q.pop(value));
    q.push(1);
    q.clear();
    assert(!q.pop(value));
    puts("web policy tests passed");
}
