// SPDX-License-Identifier: GPL-3.0-or-later
// [AT-04-R | 2026-11-25 +08:00] Offline study only, not a renderer target.
#include "../../../Source/Edition/AdaptiveToneShot.h"
#include "../../../Source/Edition/SvdLutB2AdaptiveLuma.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <random>
#include <cassert>
using namespace edition::adaptive_tone;
// Use the actual settled AT-03 eligibility and curve as baseline.
std::array<Curve,3> Candidates(const Observation& o) {
    const auto id=Identity();
    const auto r=Analyze(o.histogram);
    auto a=GuardedToneProposal(r);
    float proposalDelta=0;for(size_t i=0;i<=N;++i)proposalDelta=std::max(proposalDelta,std::abs(a[i]-id[i]));
    if(proposalDelta<=1.f/255) a=id;
    float maxDelta=0;for(size_t i=0;i<=N;++i)maxDelta=std::max(maxDelta,std::abs(a[i]-id[i]));
    const float pivot=std::clamp(r.median,.35f,.72f);
    // Separate pivot contrast from pivot brightness. No target median or white/black clipping.
    const float gain=std::min(.50f,2.5f*maxDelta);
    const float lift=.45f*(Map(a,pivot)-pivot);
    Curve b{},c{};
    for(size_t i=0;i<=N;++i){
        const float x=id[i],window=16*x*x*(1-x)*(1-x);
        c[i]=x+window*(gain*(x-pivot)+lift);
        b[i]=x+.50f*(a[i]-x)+.50f*(c[i]-x);
    }
    // Project displacement towards identity until all sampled slopes meet bounds.
    // Endpoints unchanged; no hard toe/shoulder clips. Engineering bounds, not perceptual proof.
    for(Curve* q:{&b,&c}){
        float scale=1;
        for(size_t i=1;i<=N;++i){float d=((*q)[i]-(*q)[i-1])*N-1;
            if(d>0)scale=std::min(scale,.60f/d);if(d<0)scale=std::min(scale,.35f/(-d));}
        for(size_t i=0;i<=N;++i)(*q)[i]=id[i]+scale*((*q)[i]-id[i]);
        q->front()=0;q->back()=1;
    }
    const auto production=MidtoneCurve(a,r.median);
    for(size_t i=0;i<=N;++i) assert(std::abs(production[i]-b[i])<1e-6f);
    return {a,production,c};
}
int main(int argc,char**argv){
    if(argc==2){ // actual B2.1 estimator, all four packed variants
        svdlut::b2::refined::SourceSamples samples{};std::vector<float> lut(svdlut::b2::Values);
        std::ifstream f(argv[1],std::ios::binary);f.read(reinterpret_cast<char*>(samples.data()),sizeof(samples));f.read(reinterpret_cast<char*>(lut.data()),lut.size()*4);if(!f)return 2;
        std::cout<<std::setprecision(9)<<svdlut::b2::luma::Analyze(samples,lut).tone.gains[0]<<'\n';return 0;
    }
    if(argc==3){Observation o;for(auto& x:o.histogram)if(!(std::cin>>x))return 2;for(auto& c:Candidates(o)){for(float x:c)std::cout<<std::setprecision(9)<<x<<' ';std::cout<<'\n';}return 0;}
    std::mt19937 rng(402);std::uniform_real_distribution<float> u(0,1);
    for(int n=0;n<10000;++n){Observation o;int lo=int(u(rng)*180),hi=lo+1+int(u(rng)*(255-lo));for(int i=lo;i<=hi;++i)o.histogram[i]=1+int(1000*u(rng));auto cs=Candidates(o);
        for(int k=0;k<3;++k){auto& c=cs[k];assert(c.front()==0&&c.back()==1);for(size_t i=1;i<=N;++i){float d=(c[i]-c[i-1])*N;assert(d>=-1e-4);if(k)assert(d>=.6499&&d<=1.6001);}for(int i=0;i<10;++i){auto v=Apply(c,{u(rng),u(rng),u(rng)});for(auto x:v)assert(std::isfinite(x)&&x>=0&&x<=1);}}}
    std::cout<<"PASS 10000 histograms, 300000 RGB applications, endpoints and B/C slope bounds\n";
}
