// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Depth/DepthStreamContract.h"
#include <cassert>
#include <iostream>
#include <random>
int main() {
    using namespace depthstream;
    assert(Present("past_filled") == "present_filled");
    assert(Present("past_keys_5") == "present_keys_5");
    for (const auto* invalid : {"past_", "present_keys_5", "image", "past"}) {
        bool rejected = false;
        try { (void)Present(invalid); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    assert(NeedsReset(false, 1, 100, 1, 200, -1));
    assert(NeedsReset(true, 1, 100, 2, 200, -1));
    assert(NeedsReset(true, 1, 100, 1, 100, -1));
    assert(NeedsReset(true, 1, 100, 1, 99, -1));
    assert(!NeedsReset(true, 1, 100, 1, 2500100, -1));
    assert(NeedsReset(true, 1, 100, 1, 2500101, -1));
    assert(NeedsReset(true, 1, 100, 1, 200, 200));
    assert(!NeedsReset(true, 1, 100, 1, 200, 100));
    assert(!NeedsReset(true, 1, 100, 1, 200, 201));
    BankCycle cycle;
    std::mt19937 random(1234);
    // Model a stateful dispatch where output depends on input state. Exercise
    // production bank-selection logic, including odd/even reset boundaries.
    unsigned physical[2]{};
    unsigned reference = 0;
    unsigned resets = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        const bool reset = random() % 13 == 0;
        if (cycle.Begin(reset)) {
            physical[cycle.input] = 0;
            reference = 0;
            ++resets;
        }
        assert(cycle.input != cycle.Output());
        assert(physical[cycle.input] == reference);
        const unsigned frame = random() % 100;
        reference = reference * 33u + frame;
        physical[cycle.Output()] = physical[cycle.input] * 33u + frame;
        cycle.Advance();
        assert(physical[cycle.input] == reference);
    }
    std::cout << "100000 bank transitions, " << resets << " resets; timeline boundary and state names PASS\n";
}
