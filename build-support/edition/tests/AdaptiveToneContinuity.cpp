// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include "Edition/AdaptiveToneShot.h"

namespace {
using REFERENCE_TIME=int64_t; using CRefTime=int64_t;
constexpr int64_t INVALID_TIME=INT64_MIN; constexpr int State_Running=2;
#define FAILED(x) ((x)<0)
struct CRect {int l=0,t=0,r=1920,b=1080; CRect()=default; CRect(int a,int c,int d,int e):l(a),t(c),r(d),b(e){} bool operator==(const CRect&)const=default; bool IntersectRect(CRect a,CRect b){l=std::max(a.l,b.l);t=std::max(a.t,b.t);r=std::min(a.r,b.r);this->b=std::min(a.b,b.b);return l<r&&t<this->b;}};
namespace mfg { constexpr unsigned MaxGenerated=4; }
uint64_t tick=100; uint64_t GetTickCount64(){return tick;}
struct CDX11VideoProcessor {
 struct Frame {struct Image {struct Desc {unsigned Width=1920,Height=1080,Format=87;}desc;bool pShaderResource=true;}image; CRect picture;int64_t pts=0,due=1000000;uint64_t fence=10;unsigned phase=mfg::MaxGenerated;bool projected=false,videoOnly=true,sourceColorFinal=false;};
 struct A {edition::adaptive_tone::Shot shot;uint64_t primeTick=0;bool pending=false;}m_adaptiveTone;
 std::deque<Frame> m_completedFrames;std::deque<int> m_completedBatches;
 struct Fence{uint64_t value=10;uint64_t GetCompletedValue(){return value;}}fence;Fence* m_completedFence=&fence;
 bool m_pDeviceContext=true,m_fastShutdown=false,m_extendedStartupPending=false,geometry=false,supported=true,mayStart=true;
 struct Fair {bool active=false;bool Active(){return active;}}m_completedPresentFairness;
 struct Filter {int m_filterState=State_Running;bool m_bFlushing=false;std::atomic<bool> m_completedSourceWaiting{false};int64_t stream=0;int result=0;int StreamTime(CRefTime& s){s=stream;return result;}}filter;Filter* m_pFilter=&filter;
 int64_t ready=20000000, request=INVALID_TIME, limit=INVALID_TIME, lastHead=INVALID_TIME; unsigned captures=0,polls=0;bool lockOnPoll=false;
 bool AdaptiveToneSupported(){return supported;}bool CompletedGeometryChanging(){return geometry;}bool VideoEnhancementMayStart(bool){return mayStart;}
 struct Time {int64_t ready;}; Time GetCompletedBufferTime(CRefTime){return {ready};}
 bool AdaptiveToneCaptureAllowed(int64_t pts,bool){return !m_adaptiveTone.pending&&(request==INVALID_TIME||pts-request>=800000);}
 void PollAdaptiveTone(int64_t p,int64_t l){++polls;lastHead=p;limit=l;if(lockOnPoll)m_adaptiveTone.shot.locked=1;}
 void ObserveAdaptiveTone(const Frame::Image&,const CRect&,int64_t pts,bool buffered,bool primed){assert(buffered&&primed);++captures;request=pts;m_adaptiveTone.pending=true;}
 void PrimeAdaptiveToneFrames();
};

#include "AdaptiveTonePriming.generated.inc"

void Exercise(){using P=CDX11VideoProcessor;P p;
 p.m_completedFrames.emplace_back();auto& q=p.m_completedFrames;
 q[0].phase=0;for(int i=1;i<=15;++i){auto f=q[0];f.pts=i*800000;f.phase=mfg::MaxGenerated;q.push_back(f);}q[1].fence=11;q[2].image.desc.Format=28;
 p.PrimeAdaptiveToneFrames();assert(p.captures==0&&p.polls==1); // no crossing unfinished fence
 q[1].fence=10;q[1].phase=0;tick+=33;p.PrimeAdaptiveToneFrames();assert(p.captures==0&&p.polls==2); // no crossing changed format
 q[2].image.desc.Format=87;tick+=33;p.PrimeAdaptiveToneFrames();assert(p.captures==1&&p.request==1600000&&p.limit==10000000&&p.lastHead==0);
 p.m_adaptiveTone.pending=false;p.PrimeAdaptiveToneFrames();assert(p.captures==1&&p.polls==3);
 tick+=33;p.PrimeAdaptiveToneFrames();assert(p.captures==2&&p.request==2400000);
 auto block=[](auto configure){P x;x.m_completedFrames.emplace_back();configure(x);x.PrimeAdaptiveToneFrames();assert(!x.captures&&!x.polls);};
 block([](P&x){x.geometry=true;});block([](P&x){x.m_completedPresentFairness.active=true;});block([](P&x){x.m_completedBatches.push_back(1);});block([](P&x){x.filter.m_completedSourceWaiting=true;});block([](P&x){x.ready=2499999;});block([](P&x){x.filter.stream=950000;});block([](P&x){x.filter.m_bFlushing=true;});block([](P&x){x.m_fastShutdown=true;});block([](P&x){x.supported=false;});block([](P&x){x.m_extendedStartupPending=true;});block([](P&x){x.m_adaptiveTone.shot.locked=1;});block([](P&x){x.fence.value=UINT64_MAX;});block([](P&x){x.filter.result=-1;});block([](P&x){x.mayStart=false;});
 P x;x.m_completedFrames.emplace_back();x.lockOnPoll=true;x.PrimeAdaptiveToneFrames();assert(x.polls==1&&!x.captures);
 P w;w.supported=false;w.m_completedFrames.emplace_back();w.PrimeAdaptiveToneFrames();assert(!w.captures&&!w.polls);
 P z;z.m_adaptiveTone.shot.locked=1;z.m_completedFrames.emplace_back();z.PrimeAdaptiveToneFrames();assert(!z.captures&&!z.polls); // no future AT cut after lock
 std::cout<<"PASS AT-06 production priming: pressure, deadline, reserve, fence, format, source waiter, cadence, locked shot\n";
}
}

void TestAdaptiveToneContinuity() { Exercise(); }
