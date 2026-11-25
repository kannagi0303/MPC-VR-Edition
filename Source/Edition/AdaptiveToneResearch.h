// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// [AT-01 | 2026-11-25 11:05 +08:00] Offline research candidate, NOT connected to playback.
// Full-range SDR Rec.709 code luma. No dependency on SVDLUT or renderer state.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
namespace edition::adaptive_tone {
constexpr size_t N = 256;
using Histogram = std::array<uint32_t,N>;
using Curve = std::array<float,N+1>;
inline float Unit(float x) { return (std::clamp)(x,0.f,1.f); }
inline float Smooth(float a,float b,float x) { const float t=Unit((x-a)/(b-a)); return t*t*(3-2*t); }
inline Curve Identity() { Curve c{}; for(size_t i=0;i<c.size();++i) c[i]=float(i)/N; return c; }
inline float Quantile(const Histogram& h, uint64_t count, double q) {
    const double rank=q*count; uint64_t sum=0;
    for(size_t i=0;i<N;++i) { if(h[i] && double(sum+h[i])>=rank) return Unit(float((i+(rank-sum)/h[i])/N)); sum+=h[i]; }
    return 1.f;
}
struct Result { Curve curve=Identity(); float black=0, white=1, median=0, strength=0, peak=0; };
inline Result Analyze(const Histogram& h) {
    Result r; uint64_t count=0; uint32_t peak=0;
    for(auto v:h) { count+=v; peak=(std::max)(peak,v); }
    if(count<256) return r;
    const float b=Quantile(h,count,.01), w=Quantile(h,count,.99), m=Quantile(h,count,.5);
    const float inner=Quantile(h,count,.9)-Quantile(h,count,.1);
    // 4-bin mass detects nearly flat backgrounds despite minor noise.
    uint64_t local=0, maxLocal=0;
    for(size_t i=0;i<N;++i) {local+=h[i]; if(i>=4) local-=h[i-4]; maxLocal=(std::max)(maxLocal,local);}
    r.black=b; r.white=w; r.median=m; r.peak=float(maxLocal)/count;
    // Conservative heuristic, not a published or trained classifier.
    // Do not force high/low-key, graphics, or already broad-range scenes to fill the range.
    const float s=.65f*(1-Smooth(.52f,.70f,w-b))*Smooth(.08f,.14f,inner)
        *(1-Smooth(.35f,.65f,r.peak))*Smooth(.12f,.27f,m)*(1-Smooth(.78f,.91f,m));
    if(w-b<.08f || b<.001f || w>.999f) return r;
    const float b2=b-(std::min)(.16f,b*s), w2=w+(std::min)(.12f,(1-w)*s);
    r.strength=s;
    if(s<=0) return r;
    const std::array<float,4> x{0,b,w,1}, y{0,b2,w2,1};
    std::array<float,3> d{}; for(size_t j=0;j<3;++j) d[j]=(y[j+1]-y[j])/(x[j+1]-x[j]);
    const std::array<float,4> slope{d[0],(std::min)(d[0],d[1]),(std::min)(d[1],d[2]),d[2]};
    for(size_t i=0;i<=N;++i) {
        const float v=float(i)/N; const size_t j=v<b?0:v<w?1:2; const float dx=x[j+1]-x[j],t=Unit((v-x[j])/dx),t2=t*t,t3=t2*t;
        r.curve[i]=Unit((2*t3-3*t2+1)*y[j]+(t3-2*t2+t)*dx*slope[j]+(-2*t3+3*t2)*y[j+1]+(t3-t2)*dx*slope[j+1]);
    }
    r.curve.front()=0; r.curve.back()=1; return r;
}
inline float Map(const Curve& c,float y) {const float p=Unit(y)*N; const auto i=(std::min)(size_t(p),N-1); return c[i]+(p-i)*(c[i+1]-c[i]);}
// Filter the correction curve, NOT source brightness. Caller must supply verified
// shot boundaries, flash/fade decisions and timeline discontinuities.
struct Temporal {
    Curve curve=Identity(); bool valid=false;
    void Reset(){curve=Identity();valid=false;}
    Curve Update(const Curve& target,float dt,bool cut,bool transient) {
        if(cut || !std::isfinite(dt) || dt<0 || dt>1) Reset();
        if(transient) { Reset(); return Identity(); }
        if(!valid) {curve=target;valid=true;return curve;}
        const float a=1-std::exp(-dt/.30f);
        for(size_t i=0;i<=N;++i) curve[i]+=a*(target[i]-curve[i]);
        return curve;
    }
};
// Source-relative chroma ray; preserve requested code-luma without RGB clipping.
inline std::array<float,3> Apply(const Curve& c,std::array<float,3> rgb) {
    const float y=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2], z=Map(c,y);
    float scale=1; for(float v:rgb){const float delta=v-y; if(delta>0) scale=(std::min)(scale,(1-z)/delta); else if(delta<0) scale=(std::min)(scale,-z/delta);}
    for(auto& v:rgb) v=Unit(z+scale*(v-y));
    return rgb;
}
}
