// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AdaptiveToneCurve.h"
// [AT-03 | 2026-11-25 11:25 +08:00] Shot anchor, not a frame-by-frame AGC.
namespace edition::adaptive_tone {
constexpr unsigned SampleWidth=256, SampleHeight=144, TileSize=8, AnchorSamples=10;
struct Observation { Histogram histogram{}; std::array<float,TileSize*TileSize*3> tiles{}; };
inline Observation ReadObservation(const float* pixels, size_t strideFloats) {
    Observation o;
    for(unsigned y=0;y<SampleHeight;++y) for(unsigned x=0;x<SampleWidth;++x) {
        const float* p=pixels+y*strideFloats+4*x;
        const float r=std::isfinite(p[0])?Unit(p[0]):0, g=std::isfinite(p[1])?Unit(p[1]):0, b=std::isfinite(p[2])?Unit(p[2]):0;
        ++o.histogram[(std::min)(255u,unsigned(256*Unit(.2126f*r+.7152f*g+.0722f*b)))];
        constexpr float inv=1.f/((SampleWidth/TileSize)*(SampleHeight/TileSize));
        const auto i=3*((y/(SampleHeight/TileSize))*TileSize+x/(SampleWidth/TileSize)); o.tiles[i]+=r*inv; o.tiles[i+1]+=g*inv; o.tiles[i+2]+=b*inv;
    }
    return o;
}
struct Change { bool cut=false, illumination=false; };
inline Change Compare(const Observation& a,const Observation& b) {
    // Distinguish global affine changes from composition changes. A heuristic,
    // not semantic cut/flash detection. Fixed curve preserves fade direction.
    double sumA=0,sumB=0,aa=0,ab=0,err=0,color=0;
    constexpr double n=TileSize*TileSize*3;
    for(size_t i=0;i<a.tiles.size();++i){double x=a.tiles[i],y=b.tiles[i];sumA+=x;sumB+=y;aa+=x*x;ab+=x*y;color+=std::abs(x-y);}
    const double variance=aa-sumA*sumA/n;
    const double gain=variance>1e-5?(ab-sumA*sumB/n)/variance:1;
    const double offset=(sumB-gain*sumA)/n;
    for(size_t i=0;i<a.tiles.size();++i) err+=std::abs(b.tiles[i]-gain*a.tiles[i]-offset);
    double cdf=0,distance=0; for(size_t i=0;i<N;++i){cdf+=(double(b.histogram[i])-a.histogram[i])/(SampleWidth*SampleHeight);distance+=std::abs(cdf)/N;}
    bool affine=err/n<.035 && gain>=0 && gain<=3 && (std::abs(gain-1)>.07 || std::abs(offset)>.035);
    return {!affine && (color/n>.24 || (color/n>.12 && distance>.10)),affine};
}
// [AT-05 | 2026-11-25 12:03 +08:00] Production AT-B: bounded midtone curve.
// Keep AT-03 eligibility; use its guarded displacement as a proposal, not
// as the final tone mapping. No runtime A/B selector or strength setting.
inline Curve GuardedToneProposal(const Result& result) {
    const float trust=Smooth(.055f,.15f,result.black)*(1-Smooth(.48f,.60f,result.black))
        *Smooth(.22f,.35f,result.median)*(1-Smooth(.73f,.84f,result.median));
    const auto identity=Identity();
    Curve base{};
    for(size_t j=0;j<=N;++j) base[j]=identity[j]+trust*(result.curve[j]-identity[j]);
    return base;
}
inline Curve MidtoneCurve(const Curve& base,float median) {
    const auto id=Identity();
    float maxDelta=0;
    for(size_t i=0;i<=N;++i) maxDelta=(std::max)(maxDelta,std::abs(base[i]-id[i]));
    if(maxDelta<=1.f/255) return id; // preserve AT-04's inactive reference cases
    const float pivot=(std::clamp)(median,.35f,.72f);
    const float gain=(std::min)(.50f,2.5f*maxDelta);
    const float lift=.45f*(Map(base,pivot)-pivot);
    Curve result{};
    for(size_t i=0;i<=N;++i) {
        const float x=id[i],window=16*x*x*(1-x)*(1-x);
        const float mid=x+window*(gain*(x-pivot)+lift);
        result[i]=x+.50f*(base[i]-x)+.50f*(mid-x);
    }
    // Project towards identity until every piecewise-linear interval has
    // slope in [.65,1.60]. Pointwise median and activation preserve these bounds.
    float scale=1;
    for(size_t i=1;i<=N;++i) {
        const float d=(result[i]-result[i-1])*N-1;
        if(d>0) scale=(std::min)(scale,.60f/d);
        if(d<0) scale=(std::min)(scale,.35f/(-d));
    }
    for(size_t i=0;i<=N;++i) result[i]=id[i]+scale*(result[i]-id[i]);
    result.front()=0; result.back()=1;
    return result;
}
struct Shot {
    Curve curve=Identity(); Observation previous{}, learningStart{};
    std::array<Curve,AnchorSamples> candidates{};
    int64_t last=-1, start=-1, locked=-1, activation=-1; unsigned count=0; bool hasPrevious=false, active=false;
    void Reset(){*this=Shot{};}
    // [AT-06 | 2026-11-25 12:18 +08:00] Explicit timeline resets remain
    // authoritative. A known pressure-induced sampling gap is not itself a cut
    // for an established curve; unfinished anchors still require continuous evidence.
    // A prefetched anchor may activate no earlier than its first observation
    // and the current presentation head (never borrow a future shot).
    void Observe(const Observation& o,int64_t pts,bool resumeAfterPressure=false,int64_t presentationHead=-1) {
        if(pts<0 || (hasPrevious && pts<=last)) return;
        const auto change=hasPrevious?Compare(previous,o):Change{};
        if(hasPrevious && (change.cut || (pts-last>12'000'000 && (!resumeAfterPressure || locked<0)))) Reset();
        previous=o; last=pts; hasPrevious=true;
        if(locked>=0) return; // do not chase framing or fades once established
        if(change.illumination || (count>=3 && Compare(learningStart,o).illumination)) {count=0;start=-1;return;}
        if(count==0) {start=pts;learningStart=o;}
        const auto result=Analyze(o.histogram);
        const auto identity=Identity();
        // Bound each observation before the median, so varied framing still
        // produces a bounded shot curve; do not change the shot/cut policy.
        if(count<AnchorSamples) candidates[count++]=MidtoneCurve(GuardedToneProposal(result),result.median);
        if(count<AnchorSamples || pts-start<7'000'000) return;
        // Pointwise median of monotone maps is monotone. No per-frame curve drift.
        for(size_t j=0;j<=N;++j){std::array<float,AnchorSamples> v{};for(unsigned i=0;i<AnchorSamples;++i)v[i]=candidates[i][j];std::sort(v.begin(),v.end());curve[j]=.5f*(v[4]+v[5]);}
        locked=pts; activation=presentationHead<0 ? pts : (std::max)(start,presentationHead); active=false;
        for(size_t j=0;j<=N;++j) if(std::abs(curve[j]-identity[j])>1.f/255){active=true;break;}
    }
    // [EXP-FADE-0] Historical method name; binary activation, not a ramp.
    // Preserve the shot/PTS boundary, including a prefetched future anchor.
    float Fade(int64_t pts)const {return active && locked>=0 && activation>=0 && pts>=activation ? 1.f:0.f;}
};
}
