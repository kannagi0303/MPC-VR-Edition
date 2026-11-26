// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/CompletedBufferPolicy.h"
#undef NDEBUG
#include <cassert>
#include <iostream>

using edition::CompletedBufferPolicy;
constexpr int64_t reserve = CompletedBufferPolicy::StartupLeadMs * 10'000;

struct Probe {
    bool pending = true;
    int64_t ready = 2'000 * 10'000;
    unsigned normal = 0, forced = 0, reads = 0;
    bool Run() {
        return CompletedBufferPolicy::TryResizeBankRecovery(
            [&](bool force) {
                if (force) { ++forced; return true; }
                ++normal;
                return !pending;
            }, [&] { ++reads; return ready; }, reserve);
    }
};
int TestResizeBankRecovery() {
    // Ready batch always uses the normal path, regardless of buffer level.
    for (const auto ready : {int64_t(0), reserve - 1, reserve, reserve + 1}) {
        Probe p; p.pending = false; p.ready = ready;
        assert(p.Run() && p.normal == 1 && p.forced == 0 && p.reads == 0);
    }
    // Healthy buffer covers producer retries: no forced retirement while pending.
    Probe p;
    for (int i = 0; i < 100; ++i) assert(!p.Run());
    assert(p.normal == 100 && p.forced == 0 && p.reads == 100);
    p.pending = false;
    assert(p.Run() && p.forced == 0);
    // Exact boundary, then delayed depletion, then refill: no sticky forced mode.
    p.pending = true; p.ready = reserve;
    assert(!p.Run());
    p.ready = reserve - 1;
    assert(p.Run() && p.forced == 1);
    p.ready = 0;
    assert(p.Run() && p.forced == 2);
    p.ready = reserve + 1;
    assert(!p.Run() && p.forced == 2);
    // Repeated resize-armed bank-pressure episodes, with recoverable GPU work
    // completing on later retries; no early generated-phase retirement.
    Probe q;
    for (int i = 0; i < 5; ++i) {
        q.pending = true;
        for (int j = 0; j < 40; ++j) assert(!q.Run());
        q.pending = false;
        assert(q.Run());
    }
    assert(q.forced == 0 && q.normal == 205);
    // Synthetic delayed-drain scenario: even much later than resize, the
    // last-resort escape remains available before ready time reaches zero.
    Probe r;
    for (int64_t t = 2'000; t >= 0; t -= 10) {
        r.ready = t * 10'000;
        if (r.Run()) { assert(t == 240); break; }
    }
    assert(r.forced == 1);
    std::cout << "B08 normal-first retirement / ready reserve / delayed drain: PASS\n";
    return 0;
}
