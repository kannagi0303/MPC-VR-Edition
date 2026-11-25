// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/AdaptiveToneResearch.h"
#include <cassert>
#include <iostream>
#include <random>
using namespace edition::adaptive_tone;
int main(int argc,char**) {
    if(argc>1) { Histogram h{}; for(auto& v:h) if(!(std::cin>>v)) return 1; const auto r=Analyze(h); std::cout<<r.black<<' '<<r.white<<' '<<r.median<<' '<<r.strength<<' '<<r.peak<<'\n'; for(auto v:r.curve) std::cout<<v<<' ';return 0; }
    std::mt19937 rng(11); std::uniform_real_distribution<float> u(0,1); unsigned active=0;
    for(int i=0;i<10000;++i) { Histogram h{}; const float lo=u(rng)*.6f,hi=lo+u(rng)*(1-lo); for(int k=0;k<1024;++k) ++h[(std::min)(255,int((lo+(hi-lo)*u(rng))*256))]; auto r=Analyze(h); active+=r.strength>0; assert(r.curve.front()==0 && r.curve.back()==1); for(size_t j=1;j<=N;++j) assert(r.curve[j]>=r.curve[j-1]-1e-6f); for(int k=0;k<10;++k){ std::array<float,3> rgb{u(rng),u(rng),u(rng)}; auto out=Apply(r.curve,rgb); for(auto v:out) assert(v>=0 && v<=1); float y=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2]; assert(std::abs(.2126f*out[0]+.7152f*out[1]+.0722f*out[2]-Map(r.curve,y))<2e-6f); } }
    Histogram card{}; card[236]=10000; card[10]=100; assert(Analyze(card).curve==Identity()); Histogram empty{}; assert(Analyze(empty).curve==Identity());
    Histogram haze{}; for(int i=90;i<=210;++i) haze[i]=100; auto target=Analyze(haze).curve; assert(target!=Identity()); Temporal t; assert(t.Update(target,.04f,true,false)==target); assert(t.Update(target,.04f,false,true)==Identity()); assert(t.Update(target,.04f,false,false)==target); assert(t.Update(Identity(),.04f,true,false)==Identity());
    std::cout<<"PASS 10000 histograms; 100000 RGB probes; active="<<active<<"; identity and reset contracts\n";
}
