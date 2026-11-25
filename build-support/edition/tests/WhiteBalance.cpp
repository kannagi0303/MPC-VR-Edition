// SPDX-License-Identifier: GPL-3.0-or-later
// [WB-06 | 2026-11-25 12:55 +08:00] Production estimator and shot policy.
#include "../../../Source/Edition/WhiteBalance.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <fstream>
using namespace edition::white_balance;
int main(int argc,char** argv){
    constexpr unsigned W=edition::adaptive_tone::SampleWidth,H=edition::adaptive_tone::SampleHeight;
    std::vector<float> image(W*H*4,1.f); Scratch scratch;
    if(argc==2){std::ifstream file(argv[1],std::ios::binary);file.read(reinterpret_cast<char*>(image.data()),image.size()*sizeof(float));assert(file);auto e=scratch.Read(image.data(),W*4);std::cout<<e.valid<<' '<<e.cast[0]<<' '<<e.cast[1]<<' '<<e.cast[2]<<'\n';return 0;}
    const RGB cast{1.24f,1.f,.76f};
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const float v=.25f+.09f*std::sin(.1f*x)+.06f*std::cos(.2f*y);for(unsigned c=0;c<3;++c)image[4*(y*W+x)+c]=Encode(v*cast[c]);}
    auto e=scratch.Read(image.data(),W*4);assert(e.valid);float m=(std::log(cast[0])+std::log(cast[1])+std::log(cast[2]))/3;
    for(unsigned c=0;c<3;++c)assert(std::abs(e.cast[c]-(std::log(cast[c])-m))<.001f);
    auto gain=Gains(e.cast,100);auto corrected=Correct({image[0],image[1],image[2]},gain);assert(std::abs(corrected[0]-corrected[2])<.001f);
    auto observation=edition::adaptive_tone::ReadObservation(image.data(),W*4);Shot shot;
    // WB-07: startup 2 consistent high-confidence samples, or 3 otherwise.
    auto good=e;good.confidence=1;auto weak=e;weak.confidence=.5f;
    shot.Observe(observation,good,0);assert(!shot.locked);
    shot.Observe(observation,good,800000);assert(shot.locked);
    auto locked=shot.cast;shot.Observe(observation,{{-1,1,0},true},40'000'000);assert(shot.locked&&shot.cast==locked);
    shot.Observe(observation,{{-1,1,0},true},1);assert(shot.cast==locked);
    shot.Reset();assert(!shot.locked&&!shot.count&&Gains(shot.cast,0)==Identity);
    for(int i=0;i<2;++i){shot.Observe(observation,weak,i*800000);assert(!shot.locked);}
    shot.Observe(observation,weak,1600000);assert(shot.locked);
    // Future samples cannot activate or reset an earlier picture.
    DeferredSamples deferred;shot.Reset();assert(deferred.Push(observation,good,800000));assert(deferred.Push(observation,good,1600000));
    assert(!deferred.Push(observation,good,1600000));deferred.Consume(shot,0);assert(!shot.count&&deferred.count==2);
    deferred.Consume(shot,800000);assert(shot.count==1&&!shot.locked);deferred.Consume(shot,1599999);assert(!shot.locked);
    deferred.Consume(shot,1600000);assert(shot.locked&&!deferred.count);deferred.Reset();shot.Reset();
    assert(deferred.Push(observation,good,800000));deferred.Consume(shot,13000001);assert(!shot.count&&!deferred.count); // expired after pressure
    assert(deferred.Push(observation,good,14000000));deferred.Reset();deferred.Consume(shot,14000000);assert(!shot.count); // reset cancels pending data
    // A disagreement restarts tentative evidence rather than locking a wrong first sample.
    shot.Observe(observation,good,0);shot.Observe(observation,{{-.4f,.1f,.3f},true,1},800000);assert(shot.count==1&&!shot.locked);
    shot.Reset();
    std::mt19937 rng(16);std::uniform_real_distribution<float> u(0,1),v(-1,1);
    for(int i=0;i<100000;++i){RGB rgb{u(rng),u(rng),u(rng)},log{v(rng),v(rng),v(rng)};assert(Correct(rgb,Gains(log,0))==rgb);auto q=Correct(rgb,Gains(log,i%101));RGB a{},b{};for(unsigned c=0;c<3;++c){assert(std::isfinite(q[c])&&q[c]>=0&&q[c]<=1);a[c]=Decode(rgb[c]);b[c]=Decode(q[c]);}assert(std::abs(Dot(a,Luma)-Dot(b,Luma))<2e-6f);}
    std::fill(image.begin(),image.end(),1.f);assert(!scratch.Read(image.data(),W*4).valid);
    std::fill(image.begin(),image.end(),.5f);assert(!scratch.Read(image.data(),W*4).valid);
    // A real cut clears the old anchor; a known long sampling gap does not.
    shot.Reset();edition::adaptive_tone::Observation a{},b{};a.histogram[32]=W*H;b.histogram[220]=W*H;for(size_t i=0;i<a.tiles.size();++i){a.tiles[i]=i%2?.1f:.2f;b.tiles[i]=i%2?.9f:.6f;}
    for(int i=0;i<3;++i)shot.Observe(a,weak,i*800000);assert(shot.locked);assert(edition::adaptive_tone::Compare(a,b).cut);shot.Observe(b,weak,10*800000);assert(!shot.locked&&shot.count==1);
    // Relative evidence against a fixed reference, never accumulated frame-to-frame.
    auto make=[](RGB gain,float exposure=1.f,unsigned shift=0){edition::adaptive_tone::Observation o{};
        for(unsigned i=0;i<64;++i){unsigned j=(i+shift)%64;const float lum=.12f+.004f*j;for(unsigned c=0;c<3;++c)o.tiles[3*i+c]=Encode(lum*gain[c]*exposure);}
        o.histogram[120]=W*H;return o;};
    const RGB baseGain{1.1f,1.f,.9f},lightGain{.92f,1.13f,1.06f};
    auto a0=make(baseGain),a1=make(lightGain),ex=make(baseGain,.7f),moved=make(baseGain,1.f,31);RGB d{};
    assert(RelativeCast(a0,a1,d));RGB truth{};for(unsigned c=0;c<3;++c)truth[c]=std::log(lightGain[c]/baseGain[c]);truth=Center(truth);assert(Distance(d,truth)<.00001f);
    assert(RelativeCast(a0,ex,d)&&Distance(d,RGB{})<.00001f);assert(!RelativeCast(a0,moved,d));
    RGB b0{},b1{};for(unsigned c=0;c<3;++c){b0[c]=std::log(baseGain[c]);b1[c]=std::log(lightGain[c]);}b0=Center(b0);b1=Center(b1);
    shot.Reset();shot.Observe(a0,{b0,true,1},0);shot.Observe(a0,{b0,true,1},800000);assert(shot.locked);
    assert(!shot.NeedsEstimate(ex,20000000));assert(!shot.NeedsEstimate(moved,20000000));
    assert(shot.NeedsEstimate(a1,20000000));shot.Observe(a1,{b1,true,1},20000000);assert(shot.cast==b0 || Distance(shot.cast,b0)<1e-6f);
    shot.Observe(a1,{b1,true,1},21600000);assert(Distance(shot.cast,b0)<1e-6f);
    shot.Observe(a1,{b1,true,1},23200000);assert(Distance(shot.cast,b1)<1e-5f);assert(!shot.NeedsEstimate(a0,24000000));
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const float v=.25f+.09f*std::sin(.1f*x)+.06f*std::cos(.2f*y);for(unsigned c=0;c<3;++c)image[4*(y*W+x)+c]=Encode(v*cast[c]);}
    const auto start=std::chrono::steady_clock::now();double sink=0;for(int i=0;i<100;++i)sink+=scratch.Read(image.data(),W*4).cast[0];
    std::cout<<"PASS 100000 WB RGB, known cast, flat/clip rejection, fast anchor / causal prefetch / expiry / relative update / seek/gap/cut. CPU estimate ms "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/100<<" checksum "<<sink<<'\n';
}
