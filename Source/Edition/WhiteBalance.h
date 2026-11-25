// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AdaptiveToneShot.h"
#include <vector>
// [WB-06 | 2026-11-25 12:55 +08:00] Independent SDR Gray Edge anchor.
// Display-linear approximation, not recovery of RAW sensor white balance.
namespace edition::white_balance {
using RGB=std::array<float,3>;
inline constexpr RGB Identity{1,1,1};
inline constexpr RGB Luma{.2126f,.7152f,.0722f};
inline float Decode(float x){x=(std::clamp)(x,0.f,1.f);return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
inline float Encode(float x){x=(std::clamp)(x,0.f,1.f);return x<=.0031308f?12.92f*x:1.055f*std::pow(x,1.f/2.4f)-.055f;}
inline float Dot(RGB a,RGB b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline RGB Gains(RGB logCast,int percent){
    if(percent<=0)return Identity;
    float mean=0;for(auto& v:logCast){v=(std::clamp)(v,-.35f,.35f);mean+=v/3;}
    RGB g{};for(unsigned c=0;c<3;++c)g[c]=std::exp(-float((std::clamp)(percent,0,100))*.01f*(logCast[c]-mean));
    return g;
}
inline RGB Correct(RGB rgb,RGB gain){
    if(gain==Identity)return rgb;
    RGB x{},q{};for(unsigned c=0;c<3;++c){x[c]=Decode(rgb[c]);q[c]=x[c]*gain[c];}
    const float y=Dot(x,Luma), z=Dot(q,Luma);if(z<=1e-10f)return rgb;
    float fit=1;for(auto& c:q){c=c*y/z-y;if(c>0)fit=(std::min)(fit,(1-y)/c);else if(c<0)fit=(std::min)(fit,-y/c);}
    for(auto& c:q)c=Encode(y+c*fit);
    return q;
}
inline RGB Center(RGB a) {const float m=(a[0]+a[1]+a[2])/3;for(auto& x:a)x-=m;return a;}
inline float Distance(RGB a,RGB b) {float d=0;for(unsigned c=0;c<3;++c)d+=(a[c]-b[c])*(a[c]-b[c]);return std::sqrt(d);}
inline RGB Mix(RGB a,RGB b,float t) {for(unsigned c=0;c<3;++c)a[c]+=(b[c]-a[c])*t;return a;}
struct Estimate { RGB cast{}; bool valid=false; float confidence=1.f; };
// Bounded, separable 3-tap Gaussian approximation to the WB05 offline Gray Edge.
// Reuse a scratch buffer per renderer. No allocations in the per-pixel loop.
struct Scratch {
    std::vector<RGB> raw, horizontal, smooth;
    std::vector<uint8_t> valid, horizontalCount;
    Estimate Read(const float* pixels,size_t stride){
        constexpr unsigned W=adaptive_tone::SampleWidth,H=adaptive_tone::SampleHeight;
        if(!pixels||stride<W*4)return {};
        raw.resize(W*H);horizontal.resize(W*H);smooth.resize(W*H);valid.resize(W*H);horizontalCount.resize(W*H);
        static const auto decode=[] {std::array<float,4097> v{};for(unsigned i=0;i<v.size();++i)v[i]=Decode(i/4096.f);return v;}();
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)for(unsigned c=0;c<3;++c){
            const auto v=pixels[y*stride+4*x+c];const float p=std::isfinite(v)?(std::clamp)(v,0.f,1.f)*4096:0;
            const unsigned i=(std::min)(4095u,unsigned(p));raw[y*W+x][c]=decode[i]+(decode[i+1]-decode[i])*(p-i);
        }
        for(unsigned i=0;i<W*H;++i) {
            const auto& p=raw[i];valid[i]=p[0]>.008f&&p[0]<.97f&&p[1]>.008f&&p[1]<.97f&&p[2]>.008f&&p[2]<.97f;
        }
        for(unsigned y=0;y<H;++y)for(unsigned x=2;x+2<W;++x){const unsigned i=y*W+x;horizontalCount[i]=valid[i-2]+valid[i-1]+valid[i]+valid[i+1]+valid[i+2];}
        for(unsigned y=0;y<H;++y)for(unsigned x=1;x+1<W;++x)for(unsigned c=0;c<3;++c)
            horizontal[y*W+x][c]=.25f*(raw[y*W+x-1][c]+2*raw[y*W+x][c]+raw[y*W+x+1][c]);
        for(unsigned y=1;y+1<H;++y)for(unsigned x=1;x+1<W;++x)for(unsigned c=0;c<3;++c)
            smooth[y*W+x][c]=.25f*(horizontal[(y-1)*W+x][c]+2*horizontal[y*W+x][c]+horizontal[(y+1)*W+x][c]);
        // [WB-07 | 2026-11-25 13:30 +08:00] Spatially balanced edge evidence.
        // Tile consensus measures whether a single textured object dominates.
        std::array<double,3> energy{};unsigned count=0;
        std::array<std::array<double,3>,16> tileEnergy{}, witness{};
        std::array<unsigned,16> tileCount{};std::array<double,16> witnessWeight{};
        // Sample one of four spatial phases; exclude black/clipped 5x5 support.
        for(unsigned y=2;y+2<H;y+=2)for(unsigned x=2;x+2<W;x+=2){
            const unsigned i=y*W+x;
            if(horizontalCount[i-2*W]+horizontalCount[i-W]+horizontalCount[i]+horizontalCount[i+W]+horizontalCount[i+2*W]!=25)continue;
            RGB e{};float sum=0;
            for(unsigned c=0;c<3;++c){const float dx=.5f*(smooth[i+1][c]-smooth[i-1][c]),dy=.5f*(smooth[i+W][c]-smooth[i-W][c]);e[c]=dx*dx+dy*dy;sum+=e[c];}
            if(sum<1e-8f)continue;
            const unsigned tile=4*(y*4/H)+x*4/W;
            for(unsigned c=0;c<3;++c){energy[c]+=e[c];tileEnergy[tile][c]+=e[c];}
            ++tileCount[tile];
            // Bright, modest-chroma evidence is a soft witness, never a
            // semantic veto of an artistic cast. A tinted surface can fool it.
            const auto& p=smooth[i]; const float lo=(std::min)({p[0],p[1],p[2]}),hi=(std::max)({p[0],p[1],p[2]});
            const float luma=Dot(p,Luma);
            const float w=adaptive_tone::Smooth(.12f,.40f,luma)*(1-adaptive_tone::Smooth(.25f,.65f,(hi-lo)/hi));
            if(w>.01f){for(unsigned c=0;c<3;++c)witness[tile][c]+=w*p[c];witnessWeight[tile]+=w;}
            ++count;
        }
        if(count<64)return {};
        RGB cast{};float mean=0;
        for(unsigned c=0;c<3;++c){if(energy[c]<1e-8)return {};cast[c]=float(.5*std::log(energy[c]/count));mean+=cast[c]/3;}
        for(auto& c:cast)c-=mean;
        std::array<RGB,16> spatial{},neutral{};unsigned ns=0,nn=0;
        for(unsigned t=0;t<16;++t){
            if(tileCount[t]>=32 && (std::min)({tileEnergy[t][0],tileEnergy[t][1],tileEnergy[t][2]})>1e-8){RGB v{};for(unsigned c=0;c<3;++c)v[c]=float(.5*std::log(tileEnergy[t][c]));spatial[ns++]=Center(v);}
            if(witnessWeight[t]>=8){RGB v{};for(unsigned c=0;c<3;++c)v[c]=float(std::log(witness[t][c]/witnessWeight[t]));neutral[nn++]=Center(v);}
        }
        auto median=[](const auto& a,unsigned n){RGB v{};for(unsigned c=0;c<3;++c){std::array<float,16> q{};for(unsigned i=0;i<n;++i)q[i]=a[i][c];std::sort(q.begin(),q.begin()+n);v[c]=.5f*(q[(n-1)/2]+q[n/2]);}return Center(v);};
        const float spatialSpread=ns>=4?Distance(cast,median(spatial,ns)):0.f;
        float confidence=.65f;
        if(nn>=3){const auto n=median(neutral,nn);float disagreement=Distance(cast,n);
            // Independent agreement helps confidence; do not force full
            // correction towards grey from weak or inconsistent evidence.
            const float weight=.55f*(1-adaptive_tone::Smooth(.10f,.25f,Distance(n,RGB{})))*adaptive_tone::Smooth(.10f,.35f,disagreement);
            cast=Mix(cast,n,weight);confidence=1-.5f*adaptive_tone::Smooth(.12f,.55f,disagreement);
        }
        return {Center(cast),true,confidence*(1-.25f*adaptive_tone::Smooth(.12f,.4f,spatialSpread))};
    }
};
// [WB-07 | 2026-11-25 13:31 +08:00] Fast causal acquisition; bounded
// correction epochs. Stationary-tile evidence is not an optical-flow tracker.
inline bool RelativeCast(const adaptive_tone::Observation& a,const adaptive_tone::Observation& b,RGB& delta) {
    constexpr unsigned N=adaptive_tone::TileSize*adaptive_tone::TileSize;
    std::array<RGB,N> changes{};std::array<float,N> exposures{};unsigned n=0;
    for(unsigned i=0;i<N;++i){RGB x{},y{};for(unsigned c=0;c<3;++c){x[c]=Decode(a.tiles[3*i+c]);y[c]=Decode(b.tiles[3*i+c]);}
        if((std::min)({x[0],x[1],x[2],y[0],y[1],y[2]})<.025f || (std::max)({x[0],x[1],x[2],y[0],y[1],y[2]})>.9f)continue;
        RGB d{};for(unsigned c=0;c<3;++c)d[c]=std::log(y[c]/x[c]);
        exposures[n]=(d[0]+d[1]+d[2])/3;changes[n++]=Center(d);
    }
    if(n<24)return false;
    auto med=[](auto v,unsigned k){std::sort(v.begin(),v.begin()+k);return .5f*(v[(k-1)/2]+v[k/2]);};
    const float exposure=med(exposures,n);
    for(unsigned c=0;c<3;++c){std::array<float,N> q{};for(unsigned i=0;i<n;++i)q[i]=changes[i][c];delta[c]=med(q,n);}
    delta=Center(delta);unsigned agree=0;for(unsigned i=0;i<n;++i)if(Distance(changes[i],delta)<.045f && std::abs(exposures[i]-exposure)<.16f)++agree;
    // At least half of the entire screen, not just the selected support. A moving
    // camera or mixed illumination normally fails this strict global test.
    return agree>=32 && agree*5>=n*4 && Distance(delta,RGB{})<=.65f;
}
struct Shot {
    static constexpr unsigned Samples=3;
    RGB cast{};std::array<RGB,Samples> candidates{};unsigned count=0;float priorConfidence=0;
    int64_t last=-1,start=-1,lastValid=-1,committed=-1,lastEstimate=-1;bool locked=false,hasPrevious=false;
    adaptive_tone::Observation previous{}, reference{};
    unsigned adaptCount=0;RGB adaptCandidate{};
    void Reset(){*this=Shot{};}
    bool NeedsEstimate(const adaptive_tone::Observation& o,int64_t pts)const {
        if(!locked || (hasPrevious&&adaptive_tone::Compare(previous,o).cut))return true;
        RGB d{};return pts-committed>=10'000'000 && (lastEstimate<0 || pts-lastEstimate>=1'600'000) && RelativeCast(reference,o,d) && Distance(d,RGB{})>=.075f;
    }
    void Observe(const adaptive_tone::Observation& o,Estimate e,int64_t pts){
        if(pts<0||(hasPrevious&&pts<=last))return;
        const auto change=hasPrevious?adaptive_tone::Compare(previous,o):adaptive_tone::Change{};
        if(change.cut)Reset();
        previous=o;hasPrevious=true;last=pts;
        if(locked){
            if(!e.valid){RGB d{};if(!RelativeCast(reference,o,d)||Distance(d,RGB{})<.075f)adaptCount=0;return;}
            lastEstimate=pts;RGB d{};
            if(pts-committed<10'000'000 || !RelativeCast(reference,o,d) || Distance(d,RGB{})<.075f){adaptCount=0;return;}
            RGB target=cast;for(unsigned c=0;c<3;++c)target[c]+=d[c];target=Center(target);
            // Absolute estimator must corroborate a relative change; no
            // cumulative per-frame integration, no drift when evidence fails.
            if(Distance(target,e.cast)>.18f){adaptCount=0;return;}
            if(adaptCount&&Distance(adaptCandidate,target)>.07f)adaptCount=0;
            adaptCandidate=adaptCount?Mix(adaptCandidate,target,.5f):target;
            if(++adaptCount>=3){cast=Center(adaptCandidate);reference=o;committed=pts;adaptCount=0;}
            return;
        }
        if(!e.valid)return;
        if(lastValid>=0 && pts-lastValid>12'000'000){count=0;start=-1;}
        if(count&&Distance(candidates[count-1],e.cast)>.20f){count=0;start=-1;}
        lastValid=lastEstimate=pts;
        if(!count)start=pts;
        if(count==Samples){candidates[0]=candidates[1];candidates[1]=candidates[2];--count;}
        candidates[count++]=e.cast;
        // Two closely agreeing high-confidence samples: 80ms of source time.
        // Otherwise require three samples, and use a component-wise median.
        const bool early=count==2 && priorConfidence>=.8f && e.confidence>=.8f && Distance(candidates[0],candidates[1])<=.045f && pts-start>=800'000;
        priorConfidence=e.confidence;
        if(!early && (count<Samples || pts-start<1'600'000))return;
        for(unsigned c=0;c<3;++c){std::array<float,Samples> v{};for(unsigned i=0;i<count;++i)v[i]=candidates[i][c];std::sort(v.begin(),v.begin()+count);cast[c]=.5f*(v[(count-1)/2]+v[count/2]);}
        cast=Center(cast);locked=true;reference=o;committed=pts;adaptCount=0;
    }
};
// Prefetch does not advance the active WB epoch. Entries are applied only
// at a draw whose source PTS has reached the observation; seek/off clears.
struct DeferredSamples {
    struct Item {adaptive_tone::Observation observation{};Estimate estimate{};int64_t pts=-1;};
    std::array<Item,16> items{};unsigned count=0;
    void Reset(){count=0;}
    bool Push(const adaptive_tone::Observation& o,Estimate e,int64_t pts){
        if(count==items.size() || pts<0 || (count&&pts<=items[count-1].pts))return false;
        items[count++]={o,e,pts};return true;
    }
    void Consume(Shot& shot,int64_t pts){
        unsigned n=0;while(n<count&&items[n].pts<=pts){const auto& x=items[n++];if(pts-x.pts<=12'000'000)shot.Observe(x.observation,x.estimate,x.pts);}
        if(n){for(unsigned i=n;i<count;++i)items[i-n]=items[i];count-=n;}
    }
};
}
