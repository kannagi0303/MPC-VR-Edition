// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/AdaptiveToneShot.h"
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <random>


using namespace edition::adaptive_tone;
using Image=std::array<float,SampleWidth*SampleHeight*4>;
Image TestImage(float gain=1,float offset=0,bool cut=false) {
    Image a{};for(unsigned y=0;y<SampleHeight;++y)for(unsigned x=0;x<SampleWidth;++x){
        float l=.33f+.39f*((x+2*y)%64)/63.f;
        for(unsigned c=0;c<3;++c)a[4*(SampleWidth*y+x)+c]=Unit(gain*(cut?(c==0?.04f:c==1?.13f:.97f):l)+offset);
        a[4*(SampleWidth*y+x)+3]=1;
    }return a;
}
int TestAdaptiveToneShot() {
    Shot s;auto a=TestImage();auto o=ReadObservation(a.data(),SampleWidth*4);
    for(int i=0;i<10;++i)s.Observe(o,i*800000); assert(s.locked==7200000 && s.active); assert(s.Fade(s.locked-1)==0 && s.Fade(s.locked)==1 && s.Fade(s.locked+1)==1 && s.Fade(s.locked+2500000)==1);
    const auto anchor=s.curve;auto previous=s.last;s.Observe(o,s.last);assert(s.last==previous);
    for(int i=0;i<20;++i){auto f=TestImage(1.f-i/20.f);s.Observe(ReadObservation(f.data(),SampleWidth*4),(10+i)*800000);assert(s.curve==anchor);}
    for(float probe:{.25f,.5f,.75f})for(int i=0;i<100;++i)assert(Map(anchor,probe*(99-i)/100.f)<=Map(anchor,probe*(100-i)/100.f)+1e-6f);
    // distinct colour cut: old curve discarded, not blended into new shot
    auto c=TestImage(1,0,true);s.Observe(ReadObservation(c.data(),SampleWidth*4),30*800000);assert(s.locked<0 && !s.active && s.curve==Identity());
    s.Reset(); assert(s.last==-1 && s.count==0 && s.Fade(99999999)==0);
    for(int i=0;i<10;++i){auto f=TestImage(.2f+i*.06f);s.Observe(ReadObservation(f.data(),SampleWidth*4),i*800000);}assert(s.locked<0);
    // Timeline discontinuity without explicit reset cannot borrow previous anchor.
    s.Reset();for(int i=0;i<10;++i)s.Observe(o,i*800000);s.Observe(o,30000000);assert(s.locked<0 && s.count==1);
    // [AT-06] Established curves survive known pressure gaps, but never a
    // detected cut or explicit seek. An unfinished anchor is not carried across a gap.
    s.Reset();for(int i=0;i<10;++i)s.Observe(o,i*800000);
    const auto saved=s.curve; const auto lock=s.locked, activation=s.activation;
    s.Observe(o,30000000,true); assert(s.curve==saved && s.locked==lock && s.activation==activation && s.Fade(30000000)==1);
    s.Observe(ReadObservation(c.data(),SampleWidth*4),50000000,true);assert(s.locked<0 && s.Fade(50000000)==0);
    s.Reset();s.Observe(o,0);s.Observe(o,20000000,true);assert(s.count==1 && s.start==20000000);
    // Prefetch consumes only initial anchor; activate at current head, not at last future sample.
    s.Reset();for(int i=0;i<10;++i)s.Observe(o,10000000+i*800000,false,12000000);
    assert(s.locked==17200000 && s.activation==12000000 && s.Fade(11999999)==0 && s.Fade(12000000)==1 && s.Fade(12000001)==1 && s.Fade(14500000)==1);
    s.Reset();for(int i=0;i<10;++i)s.Observe(o,10000000+i*800000,false,0);
    assert(s.activation==10000000 && s.Fade(9999999)==0 && s.Fade(10000000)==1);
    s.Reset();for(int i=0;i<10;++i)s.Observe(o,10000000+i*800000,false,20000000);
    assert(s.activation==20000000 && s.Fade(19999999)==0 && s.Fade(20000000)==1);
    s.Reset();for(int i=0;i<4;++i)s.Observe(ReadObservation(c.data(),SampleWidth*4),i*800000,false,0);
    for(int i=4;i<14;++i)s.Observe(o,i*800000,false,0);
    assert(s.start==3200000 && s.locked>=0 && s.activation==s.start && s.Fade(s.start-1)==0 && s.Fade(s.start)==1);
    s.Reset();assert(s.activation<0 && s.Fade(999999999)==0);
    // Every readiness guard stays mandatory; no pre-activation or inactive curve.
    for(unsigned mask=0;mask<16;++mask){
        Shot q; q.active=bool(mask&1); q.locked=(mask&2)?200:-1;
        q.activation=(mask&4)?200:-1; const int64_t pts=(mask&8)?200:199;
        assert(q.Fade(pts)==(mask==15?1.f:0.f));
    }
    std::cout<<"PASS AT-06 pressure gap, genuine cut, seek/reset, unfinished gap, future activation boundary\n";
    // Random observations and finite, monotone luma after every accepted anchor.
    std::mt19937 rng(19);std::uniform_real_distribution<float> u(0,1);
    for (int i = 0; i < 1000; ++i) {
        s.Reset();
        const float lo = .15f + .25f * u(rng), span = .2f + .35f * u(rng);
        for (unsigned k = 0; k < SampleWidth * SampleHeight; ++k)
            for (unsigned c = 0; c < 3; ++c) a[k * 4 + c] = Unit(lo + span * u(rng));
        o = ReadObservation(a.data(), SampleWidth * 4);
        for (int k = 0; k < 10; ++k) s.Observe(o, k * 800000);
        assert(s.locked >= 0);
        for (size_t j = 1; j <= N; ++j) assert(s.curve[j] >= s.curve[j - 1] - 1e-6f);
        for (int k = 0; k < 100; ++k) {
            const float y = Map(s.curve, u(rng));
            assert(std::isfinite(y) && y >= 0 && y <= 1);
        }
    }
    // [AT-05] Pointwise median and all intermediate fade weights retain slope bounds.
    s.Reset();
    for(int i=0;i<10;++i){auto f=TestImage(1.f+.002f*i,.001f*i);s.Observe(ReadObservation(f.data(),SampleWidth*4),i*800000);}
    assert(s.locked>=0 && s.active);
    for(int k=0;k<=20;++k){float w=k/20.f;for(size_t j=1;j<=N;++j){float d=(1-w)+w*(s.curve[j]-s.curve[j-1])*N;assert(d>=.6499f && d<=1.6001f);}}
    for(int i=0;i<10000;++i){Histogram h{};int lo=int(u(rng)*180),hi=lo+1+int(u(rng)*(255-lo));for(int j=lo;j<=hi;++j)h[j]=1+int(u(rng)*1000);auto r=Analyze(h);const auto q=MidtoneCurve(GuardedToneProposal(r),r.median);assert(q.front()==0&&q.back()==1);for(size_t j=1;j<=N;++j){float d=(q[j]-q[j-1])*N;assert(d>=.6499f && d<=1.6001f);}}
    std::cout<<"PASS AT-05 production slope bounds, 10000 histograms, varied anchor, fade\n";
    std::cout<<"PASS AT-03 stable anchor, immediate activation, cut, duplicate, gap, reset; 1000 anchors, 100000 luma probes\n";
    return 0;
}
