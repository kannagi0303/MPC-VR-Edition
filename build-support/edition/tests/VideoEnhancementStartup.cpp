// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/VideoEnhancementStartup.h"
#include <cassert>
#include <iostream>
#include <limits>
using edition::ve::MayStart;
using edition::ve::Fade;
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
    // Fade is monotonic, finite, and entirely media-time based.
    constexpr int64_t start = 123456789;
    assert(Fade(start,start)==0 && Fade(start-1,start)==0);
    assert(Fade(start+1250000,start)==.5f);
    assert(Fade(start+2500000,start)==1);
    assert(Fade(std::numeric_limits<int64_t>::max(),std::numeric_limits<int64_t>::min())==1);
    float prev=0;
    for(int64_t t=0;t<=3000000;t+=1000) {float now=Fade(start+t,start);assert(now>=prev && now<=1);prev=now;}
    std::cout << "VE cold admission + media-time fade PASS\n";
}
