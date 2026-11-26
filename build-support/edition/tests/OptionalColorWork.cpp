// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include "Edition/OptionalColorWork.h"

namespace {
using CRefTime=int64_t;using REFERENCE_TIME=int64_t;
constexpr int DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709=0;
#define FAILED(x) ((x)<0)
int standalone=0,grade=0;
void StandalonePerceptualBytecode(){++standalone;}
void CachedGradeBytecode(){++grade;}
struct CDX11VideoProcessor {
 struct Cancel{bool cancelled=false;Cancel* Capture(){return this;}bool Cancelled(){return cancelled;}};
 struct Filter{bool m_bFlushing=false;std::atomic_bool m_videoEnhancement{true},m_completedSourceWaiting{false};Cancel m_playbackCancel;int64_t stream=0;int clock=0;int StreamTime(CRefTime& t){t=stream;return clock;}}filter;Filter* m_pFilter=&filter;
 struct Fence{uint64_t value=10;uint64_t GetCompletedValue(){return value;}}fence;Fence* m_completedFence=&fence;
 struct Frame{int64_t due=100000;uint64_t fence=10;};std::deque<Frame> m_completedFrames{{}};std::deque<int> m_completedBatches;
 uint64_t m_completedFenceValue=10;bool m_fastShutdown=false,m_bVPUseRTXVideoHDR=false,m_pPSHalfOUtoInterlace=false,hdr=false,geometry=false,tone=true,wb=true,m_optionalToneTurn=true;
 int m_currentSwapChainColorSpace=0, atCompile=0;
 struct Fair{bool active=false;bool Active(){return active;}}m_completedPresentFairness;
 struct B{bool failed=false;}m_svdLutRawTest;
 struct A{struct W{bool locked=false;}wb;}m_adaptiveTone;
 bool PerceptualEnabled()const{return filter.m_videoEnhancement;}
 bool SourceIsHDR(){return hdr;}bool OptionalColorSupported(){return tone;}bool WhiteBalanceSupported(){return wb;}bool CompletedGeometryChanging(){return geometry;}
 struct Pacing{double LeadMs(){return 1.0;}}m_completedDisplayPacing; int64_t CompletedBackBufferLead(const Frame&,int64_t s){return s;}
 void StartAdaptiveToneShader(){++atCompile;}
 std::string calls;int64_t toneCost=0,veCost=0;bool waiterAfterTone=false;
 void PrimeAdaptiveToneFrames(){calls+='T';filter.stream+=toneCost;if(waiterAfterTone)filter.m_completedSourceWaiting=true;}
 void PrimeSvdLutTemporalFrames(){calls+='V';filter.stream+=veCost;}
 void PrepareOptionalColorBytecode();void PrimeOptionalColorFrames();
};

#include "OptionalColorMethods.generated.inc"

void Exercise(){using P=CDX11VideoProcessor;using namespace edition::color_work;
 unsigned cases=0;for(int mask=0;mask<4096;++mask){
 P p;p.tone=mask&1;p.filter.m_videoEnhancement=mask&2;p.wb=mask&4;p.m_adaptiveTone.wb.locked=mask&8;p.m_optionalToneTurn=mask&16;
 p.m_fastShutdown=mask&32;p.filter.m_bFlushing=mask&64;p.filter.m_playbackCancel.cancelled=mask&128;p.geometry=mask&256;p.m_completedPresentFairness.active=mask&512;p.filter.m_completedSourceWaiting=mask&1024;
 if(mask&2048)p.m_completedBatches.push_back(1);
 p.PrimeOptionalColorFrames();const bool blocked=mask&4064;
 const bool t=mask&1,v=mask&2;std::string expected;
 if(!blocked){if(t&&v)expected=ToneFirst(t,v,mask&16)?"TV":"VT";else if(t)expected="T";else if(v)expected="V";}
 assert(p.calls==expected);++cases;
 }
 P p;p.toneCost=70000;p.PrimeOptionalColorFrames();assert(p.calls=="T"); // next deadline after first job
 P w;w.waiterAfterTone=true;w.PrimeOptionalColorFrames();assert(w.calls=="T");
 P q;q.m_adaptiveTone.wb.locked=true;q.m_optionalToneTurn=false;q.veCost=70000;q.PrimeOptionalColorFrames();assert(q.calls=="V");
 P r;r.m_adaptiveTone.wb.locked=true;r.PrimeOptionalColorFrames();r.PrimeOptionalColorFrames();assert(r.calls=="TVVT");
 auto block=[](auto f){P x;f(x);x.PrimeOptionalColorFrames();assert(x.calls.empty());};
 block([](P&x){x.m_completedFrames.clear();});block([](P&x){x.m_completedFence=nullptr;});block([](P&x){x.fence.value=UINT64_MAX;});block([](P&x){x.fence.value=11;});block([](P&x){x.m_completedFrames.front().fence=11;});block([](P&x){x.m_completedFenceValue=15;});block([](P&x){x.filter.clock=-1;});block([](P&x){x.filter.stream=70000;});
 for(unsigned mask=0;mask<256;++mask){P x;standalone=grade=0;x.atCompile=0;x.tone=mask&1;x.filter.m_videoEnhancement=mask&2;x.m_fastShutdown=mask&4;x.filter.m_bFlushing=mask&8;x.filter.m_playbackCancel.cancelled=mask&16;x.hdr=mask&32;x.m_bVPUseRTXVideoHDR=mask&64;x.m_pPSHalfOUtoInterlace=mask&128;x.PrepareOptionalColorBytecode();bool allow=!(mask&252);assert(x.atCompile==int(allow&&(mask&1)));assert(standalone==int(allow&&(mask&2)));assert(grade==int(allow&&(mask&2)&&std::thread::hardware_concurrency()>=4));}
 P h;h.m_currentSwapChainColorSpace=1;standalone=grade=0;h.PrepareOptionalColorBytecode();assert(!h.atCompile&&!standalone&&!grade);
 std::cout<<"PASS "<<cases<<" optional-prime combinations, 256 CPU preparation combinations, deadline/fence/cancel/recheck/rotation boundaries\n";
}
}

void TestOptionalColorWork() { Exercise(); }
