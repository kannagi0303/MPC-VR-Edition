// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/SvdLutB2AdaptiveLuma.h"
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <random>
#include <vector>
namespace b=svdlut::b2; namespace l=b::luma;
int TestSvdLutAdaptiveLuma(){
    std::vector<float> lut(b::Values); b::refined::SourceSamples samples{};
    std::mt19937 rng(121); std::uniform_real_distribution<float> u(0,1);
    for(int i=0;i<100000;++i){
        l::RGB s{u(rng),u(rng),u(rng)},g{u(rng),u(rng),u(rng)};
        const float a=u(rng)*.25f;
        auto r=l::Apply(s,g,a);float y=b::CodeLuma(s),gy=b::CodeLuma(g);
        assert(l::Apply(s,g,1)==g); assert(std::abs(b::CodeLuma(l::Apply(s,g,0))-y)<3e-7f);
        assert(std::abs(b::CodeLuma(r)-y-std::clamp(a*(gy-y),-l::MaxCodeShift,l::MaxCodeShift))<3e-7f);
        assert(std::abs(b::CodeLuma(r)-y)<=l::MaxCodeShift+3e-7f);
        for(float v:r)assert(v>=0&&v<=1);
        const auto id=l::Apply(s,s,a);for(int j=0;j<3;++j)assert(std::abs(id[j]-s[j])<3e-7f);
    }
    for(int k=0;k<30;++k){
        for(float& x:lut)x=(u(rng)-.3f)*.8f;
        for(float& x:samples)x=u(rng);
        float a=l::Analyze(samples,lut).tone.gains[0];assert(a>=0&&a<=.25f);
        for(unsigned e=0;e<4;++e){float prev=0;for(int i=0;i<=256;++i){float y=i/256.f;l::RGB s{y,y,y};float out=b::CodeLuma(l::Apply(s,b::ApplyB1(lut,s,e),a));if(i)assert((out-prev)*256>=.4999f&&(out-prev)*256<=1.5001f);prev=out;}}
    }
    assert(l::Analyze({},lut).tone.gains==l::NoLuma);
    assert(l::ShotUpdate(b::Tone{{.05f,.05f,.05f}},b::Tone{{.25f,.25f,.25f}},.1f).gains[0]==.05f);
    assert(l::ShotUpdate(b::Tone{{.25f,.25f,.25f}},b::Tone{{0,0,0}},.1f).gains[0]<.03f);
    std::cout<<"PASS 100000 composition cases, 30 random LUTs x 4 x 256 gradient steps, fallback and temporal release\n";
    return 0;
}
