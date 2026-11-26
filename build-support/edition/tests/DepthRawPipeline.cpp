// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include "Depth/DepthRawPolicy.h"
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <random>
#include <iostream>

namespace {
using std::min; using std::max;
using uint=std::uint32_t;
struct float2 { float x,y; };
struct float4 { float x,y,z,w; };
uint asuint(float v) { return std::bit_cast<uint>(v); }
float asfloat(uint v) { return std::bit_cast<float>(v); }

#include "DepthRawScalars.generated.inc"

float Normalize(float v, float lo, float hi, uint flags) {
    uint Range[3]{0,0,flags};
    return
#include "DepthRawNormalize.generated.inc"
;
}
float Expand(float depth, bool inverted) {
    if (inverted) depth=1.0f-depth;
    const float4 DepthSettings{0,0,0,0.30f};
    return
#include "DepthRawExpand.generated.inc"
;
}
void Exercise() {
    using depthraw::Usable;
    assert(Usable(0,0,0));
    assert(!Usable(-1,0,42));
    assert(!Usable(100,99,42));
    assert(Usable(100,400100,40));
    assert(!Usable(100,800100,40));
    assert(!Usable(100,101,std::numeric_limits<float>::infinity()));
    assert(!Usable(100,101,0));
    std::mt19937 rng(26);
    for (unsigned n : {67584u,307200u,1025u,1u}) {
        std::vector<float> pixels(n);
        for (auto& x : pixels) x = float(rng()%100000)/1000;
        pixels[0]=0; pixels[n-1]=200;
        auto global=RawEmpty();
        // Mirror tile dispatch and reduce all pixels through extracted math.
        for (unsigned tile=0;tile<(n+1023)/1024;++tile) {
            auto bounds=RawEmpty();
            for (unsigned lane=0;lane<256;++lane)
                for (unsigned i=0;i<4;++i) {
                    auto p=tile*1024+lane+i*256;
                    if(p<n) bounds=RawMerge(bounds,RawSample(pixels[p]));
                }
            global=RawMerge(global,bounds);
        }
        auto [lo,hi]=std::minmax_element(pixels.begin(),pixels.end());
        assert(global.x==*lo && global.y==*hi);
        for (float v : pixels) {
            const float z=Normalize(v,global.x,global.y,1);
            assert(z>=0.4399999f && z<=0.5600001f);
            assert(std::abs(Expand(z,false)+Expand(z,true)-1)<1e-6f);
        }
    }
    assert(std::abs(Normalize(0,0,4,1)-0.56f)<1e-6f);
    assert(std::abs(Normalize(4,0,4,1)-0.44f)<1e-6f);
    assert(Normalize(1,1,1,1)==0.5f);
    assert(Normalize(0,0,1,0)==0.5f);
    assert(Normalize(1,0,1,3)==Normalize(1,0,1,1)); // cut bit doesn't change depth
    for(float v : {-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        assert(!FiniteDepth(RawMerge(RawSample(1),RawSample(v)).y));
    assert(std::abs(Expand(0.44f,false)-0.35f)<1e-6f);
    assert(std::abs(Expand(0.56f,true)-0.35f)<1e-6f);
    std::cout << "PASS: fixed depth paths, PTS guards, full-map reduction (375810 samples), polarity, flat/invalid guards, consumer mapping.\n";
}
}

void TestDepthRawPipeline() { Exercise(); }
