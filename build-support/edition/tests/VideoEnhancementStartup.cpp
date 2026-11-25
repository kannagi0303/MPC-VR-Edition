// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/VideoEnhancementStartup.h"
#include <cassert>
#include <iostream>
using edition::ve::MayStart;
int main() {
    // No startup GPU work while flushing, NGX preparing, or geometry settling.
    for (int flag = 0; flag < 3; ++flag)
        assert(!MayStart(flag==0,flag==1,flag==2,true,true,true,true,20000000,120));
    assert(!MayStart(false,false,false,true,false,true,true,20000000,120));
    assert(!MayStart(false,false,false,true,true,true,false,20000000,120));
    assert(!MayStart(false,false,false,true,true,true,true,799999,3));
    assert(MayStart(false,false,false,true,true,true,true,800000,3));
    assert(MayStart(false,false,false,true,true,true,true,300000,4));
    assert(!MayStart(false,false,false,true,true,true,true,0,4));
    // Bypass and unbuffered legacy are not trapped at a fixed buffer threshold.
    assert(MayStart(false,false,false,true,true,false,false,0,0));
    assert(MayStart(false,false,false,false,false,true,false,0,0));
    // EXP-FADE-0 removes the activation ramp, not the cold admission gate.
    std::cout << "VE cold admission PASS\n";
}
