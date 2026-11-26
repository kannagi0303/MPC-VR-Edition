// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <random>
#include <vector>
#include <cstring>
#include "Edition/CompletedBufferPolicy.h"

namespace {
using REFERENCE_TIME=int64_t;
using CRefTime=int64_t;
using UINT=unsigned;
constexpr int S_OK=0, E_FAIL=-1, State_Running=2;
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
static int64_t tick=1;
static int64_t GetPreciseTick() {return ++tick;}
static double GetPreciseTicksPerSecond() {return 10000000.;}
namespace mfg {
struct CadenceTrace {
    struct CompletedQueue {size_t frames,batches; int64_t tail; double before,after; size_t samples;};
    bool Enabled() const {return true;}
    template<class... Args> void Record(Args&&...) {}
};
CadenceTrace& SourceCadenceTrace() {static CadenceTrace t;return t;}
}
struct Rect {int w=1920,h=1080;bool operator==(const Rect&) const=default;};
struct Desc {unsigned Width=1920,Height=1080,Format=87;};
static uint64_t TextureBytes(const Desc&) {return 100;}
struct Image {Desc desc;};
struct Size {int cx=1920,cy=1080;};
struct Fence {uint64_t completed=10;uint64_t GetCompletedValue() const {return completed;}};
struct Ticket {bool cancelled=false;bool Cancelled() const{return cancelled;}};
struct Cancel {Ticket t;Ticket* Capture(){return &t;}};
struct Filter {int m_filterState=State_Running;bool m_bFlushing=false;bool clockOk=true;
    REFERENCE_TIME stream=10000000;Cancel m_playbackCancel;std::atomic<bool> m_completedSourceWaiting{false};
    int StreamTime(CRefTime& x){x=stream;return clockOk?S_OK:E_FAIL;}};
struct Mfg {bool async=true;bool AsyncProducer()const{return async;}};
struct Pacing {double LeadMs()const{return 1.5;}size_t Samples()const{return 12;}};
struct CDX11VideoProcessor {
    struct CompletedFrame {Image image;Rect picture;bool videoOnly=true;uint64_t fence=10,serial=1;unsigned phase=0;int64_t due=10000000,pts=10000000;};
    struct Batch {CompletedFrame target;std::vector<unsigned> phases{0,1,2};};
    bool m_extendedStartupPending=false,m_completedFailed=false,permit=false,batchReady=true;
    unsigned m_mfgMultiplier=2,m_SwapChainFmt=87;
    std::atomic<unsigned> m_mfgEffectiveMultiplier{2};
    Size m_processingSize;Mfg m_mfg;
    std::deque<CompletedFrame> m_completedFrames{CompletedFrame{}};
    std::deque<Batch> m_completedBatches{Batch{}};
    Fence f;Fence* m_completedFence=&f;Filter filter;Filter* m_pFilter=&filter;
    Pacing m_completedDisplayPacing;
    uint64_t m_completedFenceValue=10,m_completedByteLimit=10000,committed=1000,m_mfgLostGenerated=0;
    double m_dRefreshPeriodMs=1000.0/60;
    unsigned attempts=0,composed=0;
    edition::CompletedPresentFairness m_completedPresentFairness;
    Rect ProcessingRect()const{return {m_processingSize.cx,m_processingSize.cy};}
    bool TryCompletedPresentPermit(){return permit;}
    void RefreshCompletedMemoryBudget(uint64_t){}
    struct Usage {uint64_t n;uint64_t Committed()const{return n;}};
    Usage GetCompletedPoolUsage(uint64_t)const{return {committed};}
    bool PrepareCompletedBatch(REFERENCE_TIME stream,bool force=false){
        assert(!force && stream==filter.stream);++attempts;
        if(!batchReady)return false;
        auto n=std::max(size_t(1),m_completedBatches.front().phases.size());
        m_completedBatches.pop_front();++composed;
        for(size_t i=0;i<n;++i){CompletedFrame f;f.fence=++m_completedFenceValue;m_completedFrames.push_back(f);}
        return true;
    }
    bool CompletedFairProducerCanvas()const;
    bool TryServiceCompletedProducer();
    const char* Head(REFERENCE_TIME due,REFERENCE_TIME stream,uint64_t completed,std::optional<uint64_t>& deferredFrameFence);
    bool Permit(REFERENCE_TIME remaining);
    bool Handoff(int64_t& deadline);
    bool After(int hr,int64_t started,int64_t finished);
};

#include "FairProducerMethods.generated.inc"

const char* CDX11VideoProcessor::Head(REFERENCE_TIME due,REFERENCE_TIME stream,uint64_t completed,std::optional<uint64_t>& deferredFrameFence) {
    const bool hasCompletedFrame=!m_completedFrames.empty();
    const auto margin=35000;

#include "FairAdmission.generated.inc"

    return nullptr; // only extracted head gate; all downstream gates tested separately in B09
}
bool CDX11VideoProcessor::Permit(REFERENCE_TIME remaining) {

#include "FairPermit.generated.inc"

    return true;
}
bool CDX11VideoProcessor::Handoff(int64_t& deadline) {

#include "FairHandoff.generated.inc"

    return false;
}
bool CDX11VideoProcessor::After(int hr,int64_t started,int64_t finished) {

#include "FairPresent.generated.inc"

void Exercise(){
    constexpr int64_t slowTicks=160000,fastTicks=3000;
    CDX11VideoProcessor p;
    std::optional<uint64_t> deferred;
    assert(p.Head(12000000,10000000,10,deferred)); // normal deadline
    assert(!p.Permit(1)); // normal early retry
    assert(p.Permit(-1)); // original late escape
    assert(p.After(S_OK,0,slowTicks)&&p.composed==1&&p.m_completedFrames.size()==4);
    assert(p.m_completedPresentFairness.Active());
    assert(p.m_completedPresentFairness.SourceTurnUntil(tick,true)==tick+10000);
    assert(!p.m_completedPresentFairness.SourceTurnUntil(tick,false));
    assert(!p.m_completedPresentFairness.SourceTurnUntil(tick+10000,true));
    auto deadline=tick;assert(!p.Handoff(deadline));
    p.filter.m_completedSourceWaiting=true;
    assert(p.Handoff(deadline));auto fixedDeadline=deadline;
    deadline=tick;assert(p.Handoff(deadline)&&deadline==fixedDeadline);
    deadline=fixedDeadline;assert(!p.Handoff(deadline));
    deadline=tick;p.f.completed=9;assert(!p.Handoff(deadline));p.f.completed=10;
    p.filter.stream=10200000;assert(!p.Handoff(deadline));p.filter.stream=10000000;
    assert(!p.Head(12000000,10000000,10,deferred));
    assert(p.Head(12000000,10000000,9,deferred)); // GPU not ready
    assert(p.Head(12000000,10200000,10,deferred)); // >1 refresh late
    p.m_completedPresentFairness.ConsumeAdmission();
    assert(!p.m_completedPresentFairness.SourceTurnUntil(tick,true));
    assert(p.Head(12000000,10000000,10,deferred));
    assert(!p.Permit(-1000)); // no permit + no batch: existing nonblocking retry
    assert(p.Permit(-170000)); // bounded fallback
    p.permit=true;assert(p.Permit(-1000));
    for(int i=0;i<4;++i)assert(p.After(S_OK,0,fastTicks));
    assert(!p.m_completedPresentFairness.Active());
    assert(p.composed==1);
    for(int i=0;i<13;++i){
        CDX11VideoProcessor q;
        switch(i){
        case 0:q.batchReady=false;break;
        case 1:q.m_completedBatches.front().target.videoOnly=false;break;
        case 2:q.m_completedBatches.front().target.image.desc.Width--;break;
        case 3:q.m_completedBatches.front().target.image.desc.Format++;break;
        case 4:q.m_completedFrames.front().videoOnly=false;break;
        case 5:q.f.completed=UINT64_MAX;break;
        case 6:q.m_completedFenceValue=16;break; // 6 outstanding + 3 phases >8
        case 7:q.m_completedFrames.resize(254);break;
        case 8:q.committed=9950;break;
        case 9:q.filter.m_playbackCancel.t.cancelled=true;break;
        case 10:q.filter.m_bFlushing=true;break;
        case 11:q.filter.clockOk=false;break;
        case 12:q.m_extendedStartupPending=true;break;
        }
        assert(!q.TryServiceCompletedProducer());assert(q.composed==0&&q.m_completedBatches.size()==1);
        assert(q.attempts==(i==0?1u:0u));
    }
    // An enabled MFG setting alone does not activate B11 on a bypassed source.
    CDX11VideoProcessor bypass;bypass.m_completedBatches.clear();bypass.m_mfgEffectiveMultiplier=1;
    assert(!bypass.CompletedFairProducerCanvas());
    bypass.After(S_OK,0,slowTicks);assert(!bypass.m_completedPresentFairness.Active());
    // Permit-starved branch can service a ready batch before late fallback.
    CDX11VideoProcessor q;q.m_completedPresentFairness.Observe(16,1000./60);
    assert(q.Permit(-1000)&&q.composed==1);
    // Failed present cannot grant new credit.
    q.After(E_FAIL,0,slowTicks);assert(!q.m_completedPresentFairness.Active());
    // Synthetic persistent slow-present scenario: 84 buffered frames and two
    // waiting batches, as in the actual trace. Assume ready NGX work and one
    // input admitted per needed replenishment; no Windows timing simulated.
    CDX11VideoProcessor a;a.m_completedFrames.resize(84);a.m_completedBatches.resize(2);
    size_t low=a.m_completedFrames.size();unsigned old=84;
    for(int i=0;i<240;++i){
        if(old)--old; // former scheduler: due-only presents, no batch service
        assert(!a.m_completedFrames.empty());a.m_completedFrames.pop_front();
        a.f.completed=a.m_completedFenceValue;
        a.After(S_OK,0,slowTicks);
        a.f.completed=a.m_completedFenceValue;
        // Replenish only when the simulated upstream has room (not real decoder).
        if(a.m_completedFrames.size()<84 && a.m_completedBatches.size()<2){
            std::optional<uint64_t> d;
            for(auto& f:a.m_completedFrames)f.fence=a.f.completed;
            assert(!a.Head(12000000,10000000,a.f.completed,d));
            a.m_completedPresentFairness.ConsumeAdmission();a.m_completedBatches.emplace_back();
        }
        low=std::min(low,a.m_completedFrames.size());
    }
    assert(old==0&&low>=80);
    std::cout<<"production fairness hooks PASS; 13 guard cases; synthetic 240 slow presents: old empty, candidate low="<<low<<" (assumed ready batches)\n";
}
}

void TestPresentationProducerFairness() { Exercise(); }
