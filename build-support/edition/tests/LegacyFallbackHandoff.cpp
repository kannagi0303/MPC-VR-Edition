// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include "Mfg/LiveFlowGroups.h"
#include <cassert>
#include <iostream>
#include <type_traits>

namespace {
using namespace mfg;
constexpr int64_t base=10000000, step=400000;
constexpr double hz=60., frequency=10000000.;
unsigned checks=0;
void Check(bool condition) { ++checks; assert(condition); }
const GeneratedReady ready=[](uint64_t,unsigned){return true;};
const LegacyPictureRoute direct{false,true,0,true,true};

template<class Q> void Add(Q& q,const Batch& b,int64_t qpc,int64_t stream,
    LegacyPictureRoute route={},bool native=false) {
    QueueDrops lost;
    if constexpr(std::is_same_v<Q,LiveFlowGroups>)
        lost=q.Add(b,qpc,stream,frequency,0,hz,native,3,true,false,false,{},
            LiveFlowGroups::DeadlineMode::Qpc,route);
    else lost=q.Add(b,qpc,stream,frequency,0,hz,native,3,true,{},route);
    Check(!lost.real);
}

template<class Q> void Exercise(unsigned pending,unsigned consumed,unsigned refusals,bool recovery,
    int64_t shift) {
    Q q;
    for(unsigned i=1;i<=pending;++i)
        Add(q,Batch{i,2,int64_t(i-1)*step,int64_t(i)*step,std::make_shared<OriginalSnapshot>()},
            base,0,{true,false,42},true);
    for(unsigned i=1;i<=consumed;++i) {
        QueueDrops lost;
        const auto f=q.TakeDue(base+int64_t(i)*step,lost,0,ready);
        Check(f && f->index==MaxGenerated && f->timestamp==int64_t(i)*step && !lost.real);
    }
    const auto old=q.PendingOriginalFrames();
    Check(old.size()==pending-consumed);
    const auto pendingSynthetic=q.PendingGenerated();
    Check(q.RetireLegacyRoute()==pendingSynthetic && q.PendingGenerated()==0);
    std::vector<std::shared_ptr<OriginalSnapshot>> images;
    for(unsigned i=1;i<=refusals;++i) {
        auto image=std::make_shared<OriginalSnapshot>();
        images.push_back(image);
        // A new session may not have any NGX bank. Serial=1 is a nonzero
        // admission marker, not a resource identity. A new clock anchor may
        // map the new source before an older pending original's deadline.
        Add(q,Batch{1,0,-1,int64_t(pending+i)*step,image},base,shift,direct);
        Check(q.PendingGenerated()==0);
    }
    if(recovery) {
        auto image=std::make_shared<OriginalSnapshot>();
        images.push_back(image);
        Add(q,Batch{1,0,-1,int64_t(pending+refusals+1)*step,image},base,shift,{true,false,93},true);
    }
    const auto all=q.PendingOriginalFrames();
    Check(all.size()==old.size()+images.size());
    for(size_t i=0;i<old.size();++i) {
        const auto& f=all[i];
        Check(f.original==old[i].original && f.deadline==old[i].deadline && f.timestamp==old[i].timestamp &&
            f.legacyRoute.retired && !f.legacyRoute.directFallback && f.legacyRoute.postVsrGeneration==42);
    }
    for(size_t i=0;i<images.size();++i) {
        const auto& f=all[old.size()+i];
        Check(f.original==images[i] && f.timestamp==int64_t(pending+i+1)*step);
        if(i<refusals) Check(f.legacyRoute.retired && f.legacyRoute.directFallback &&
            !f.nativeInput && !f.legacyRoute.videoOnly && f.legacyRoute.projected && !f.legacyRoute.postVsrGeneration);
        else Check(!f.legacyRoute.directFallback && !f.legacyRoute.retired && f.legacyRoute.postVsrGeneration==93);
    }
    // Late display, aggressive Prefer/Off, and recycled serial must not
    // replace any old or direct-fallback original. Even a clock reanchor
    // must not admit a later picture before the retained prefix.
    if(!old.empty()) {
        QueueDrops lost;
        Check(!q.TakeDue(old.front().deadline-1,lost,0,ready) && !lost.real);
    }
    for(const auto& f:all) {
        QueueDrops lost;
        if(f.legacyRoute.retired) {
            Check(!q.TakeReplacement(base+100*step,0,step,OriginalFrameProtection::Off,lost,ready));
            Check(!q.TakeReplacement(base+100*step,0,step,OriginalFrameProtection::Prefer,lost,ready));
        }
        auto selected=q.TakeDue(base+100*step,lost,0,ready);
        Check(selected && selected->original==f.original && selected->timestamp==f.timestamp &&
            selected->deadline==f.deadline && selected->index==MaxGenerated && !lost.real);
    }
    Check(!q.PendingOriginals() && !q.PendingGenerated());
    q.Clear();
}

template<class Q> void EqualPts() {
    Q q;
    auto a=std::make_shared<OriginalSnapshot>(), b=std::make_shared<OriginalSnapshot>();
    Add(q,Batch{1,0,-1,step,a},base,0);
    q.RetireLegacyRoute();
    Add(q,Batch{1,0,-1,step,b},base,step,direct);
    QueueDrops lost;
    auto first=q.TakeDue(base+step,lost,0,ready), second=q.TakeDue(base+step,lost,0,ready);
    Check(first && second && first->original==a && second->original==b && !lost.real);
}
}
int main() {
    for(unsigned n=1;n<=8;++n) for(unsigned used=0;used<n;++used)
        for(unsigned refused=1;refused<=4;++refused) for(bool recovery:{false,true})
            for(int64_t reanchor:{int64_t{0},step*3,step*20,-step*3}) {
                Exercise<FrameQueue>(n,used,refused,recovery,reanchor);
                Exercise<LiveFlowGroups>(n,used,refused,recovery,reanchor);
            }
    EqualPts<FrameQueue>(); EqualPts<LiveFlowGroups>();
    std::cout<<checks<<" checks passed\n";
}
