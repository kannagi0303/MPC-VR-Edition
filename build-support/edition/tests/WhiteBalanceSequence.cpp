// SPDX-License-Identifier: GPL-3.0-or-later
// [WB-07 | 2026-11-25 13:33 +08:00] Run actual C++ estimator + shot on RGBA FP32 sequences.
#include "Edition/WhiteBalance.h"
#include <fstream>
#include <iostream>
using namespace edition::white_balance;
template<class S> bool Need(const S& s,const edition::adaptive_tone::Observation& o,int64_t pts){
    if constexpr(requires{ s.NeedsEstimate(o,pts); })return s.NeedsEstimate(o,pts);
    else return !s.locked || (s.hasPrevious&&edition::adaptive_tone::Compare(s.previous,o).cut);
}
int main(int argc,char** argv){
    if(argc!=2)return 1;
    constexpr unsigned W=edition::adaptive_tone::SampleWidth,H=edition::adaptive_tone::SampleHeight;
    std::ifstream f(argv[1],std::ios::binary);if(!f)return 1;
    std::vector<float> pixels(W*H*4);Scratch scratch;Shot shot;int64_t pts=0;
    while(f.read(reinterpret_cast<char*>(pixels.data()),pixels.size()*sizeof(float))){
        auto o=edition::adaptive_tone::ReadObservation(pixels.data(),W*4);Estimate e{};
        if(Need(shot,o,pts))e=scratch.Read(pixels.data(),W*4);
        shot.Observe(o,e,pts);std::cout<<pts<<','<<shot.locked<<','<<shot.cast[0]<<','<<shot.cast[1]<<','<<shot.cast[2]<<'\n';pts+=800000;
    }
    return f.eof()&&f.gcount()==0?0:2;
}
