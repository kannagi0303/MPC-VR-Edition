// SPDX-License-Identifier: GPL-3.0-or-later
#undef NDEBUG
#include "Mfg/LiveFlowGroups.h"
#include <cassert>
#include <iostream>
#include <type_traits>

namespace {
using namespace mfg;
constexpr int64_t step = 400000, base = 10000000;
constexpr double hz = 60, frequency = 10000000;
unsigned checks = 0;
void Check(bool condition) { ++checks; assert(condition); }
const GeneratedReady ready = [](uint64_t, unsigned) { return true; };

template<class Q> void Add(Q& q, const Batch& b, bool native, LegacyPictureRoute route,
                           LiveFlowGroups::DeadlineMode mode = LiveFlowGroups::DeadlineMode::Qpc) {
    QueueDrops lost;
    if constexpr (std::is_same_v<Q, LiveFlowGroups>)
        lost = q.Add(b, base, 0, frequency, 0, hz, native, 3, true, false, false, {}, mode, route);
    else
        lost = q.Add(b, base, 0, frequency, 0, hz, native, 3, true, {}, route);
    Check(lost.real == 0);
}

template<class Q> void Exercise(unsigned count, unsigned consumed, unsigned layout, bool reuseSerial) {
    Q q;
    const LegacyPictureRoute oldRoute{bool(layout & 1), bool(layout & 2), uint64_t(layout & 4 ? 41 : 0)};
    const LegacyPictureRoute newRoute{!oldRoute.videoOnly, !oldRoute.projected, 92};
    for (unsigned n=1; n<=count; ++n) {
        Batch b{n, 2, int64_t(n-1)*step, int64_t(n)*step, std::make_shared<OriginalSnapshot>()};
        Add(q,b,bool(layout & 8),oldRoute);
    }
    for (unsigned n=1; n<=consumed; ++n) {
        QueueDrops drops;
        auto f=q.TakeDue(base+int64_t(n)*step,drops,0,ready);
        Check(f && f->index==MaxGenerated && f->timestamp==int64_t(n)*step && drops.real==0);
    }
    const auto before=q.PendingOriginalFrames();
    const auto generated=q.PendingGenerated();
    Check(before.size()==count-consumed);
    Check(q.RetireLegacyRoute()==generated);
    Check(q.PendingGenerated()==0 && q.RetireLegacyRoute()==0);
    const auto after=q.PendingOriginalFrames();
    Check(before.size()==after.size());
    for(size_t i=0;i<after.size();++i) {
        const auto& a=after[i]; const auto& b=before[i];
        Check(a.deadline==b.deadline && a.timestamp==b.timestamp && a.serial==b.serial &&
              a.original==b.original && a.nativeInput==b.nativeInput && a.presentationTime==b.presentationTime);
        Check(a.legacyRoute.retired && a.legacyRoute.videoOnly==oldRoute.videoOnly &&
              a.legacyRoute.projected==oldRoute.projected && a.legacyRoute.postVsrGeneration==oldRoute.postVsrGeneration);
    }
    // First new-route source cannot interpolate across the reset. Its serial
    // may restart at 1 when Session recreates its GPU resources.
    const uint64_t serial=reuseSerial?1:count+1;
    Add(q,Batch{serial,0,-1,int64_t(count+1)*step,std::make_shared<OriginalSnapshot>()},false,newRoute);
    Add(q,Batch{serial+1,2,int64_t(count+1)*step,int64_t(count+2)*step,std::make_shared<OriginalSnapshot>()},false,newRoute);
    for (const auto& expected:after) {
        QueueDrops drops;
        Check(!q.TakeReplacement(base+int64_t(count+3)*step,0,step,OriginalFrameProtection::Prefer,drops,ready));
        Check(!q.TakeReplacement(base+int64_t(count+3)*step,0,step,OriginalFrameProtection::Off,drops,ready));
        Check(!drops.real);
        auto f=q.TakeDue(expected.deadline,drops,0,ready);
        Check(f && f->index==MaxGenerated && f->legacyRoute.retired && f->original==expected.original &&
              f->timestamp==expected.timestamp && f->deadline==expected.deadline && drops.real==0);
    }
    QueueDrops drops;
    auto next=q.TakeDue(base+int64_t(count+1)*step,drops,0,ready);
    Check(next && next->index==MaxGenerated && !next->legacyRoute.retired && next->serial==serial &&
          next->legacyRoute.videoOnly==newRoute.videoOnly && next->legacyRoute.projected==newRoute.projected &&
          next->legacyRoute.postVsrGeneration==92 && !drops.real);
    q.Clear();
    Check(q.PendingOriginals()==0 && q.PendingGenerated()==0);
}

template<class Q> void RepeatedTransitions() {
    Q q;
    for(unsigned n=1;n<=7;++n) {
        Add(q,Batch{1,0,-1,int64_t(n)*step,std::make_shared<OriginalSnapshot>()},bool(n&1),{bool(n&1),bool(n&2),n});
        Check(q.RetireLegacyRoute()==0);
    }
    for(unsigned n=1;n<=7;++n) {
        QueueDrops lost;
        auto f=q.TakeDue(base+int64_t(n)*step,lost,0,ready);
        Check(f && f->legacyRoute.retired && f->legacyRoute.postVsrGeneration==n && f->timestamp==int64_t(n)*step && !lost.real);
    }
    Check(!q.PendingOriginals());
}
}
int main() {
    for(unsigned n=1;n<=8;++n) for(unsigned used=0;used<=n;++used)
        for(unsigned layout=0;layout<16;++layout) for(bool reuse:{false,true}) {
            Exercise<FrameQueue>(n,used,layout,reuse);
            Exercise<LiveFlowGroups>(n,used,layout,reuse);
        }
    RepeatedTransitions<FrameQueue>(); RepeatedTransitions<LiveFlowGroups>();
    LiveFlowGroups completed;
    Add(completed,Batch{1,2,0,step,std::make_shared<OriginalSnapshot>()},true,{},LiveFlowGroups::DeadlineMode::StreamTime);
    const auto phases=completed.TakeBufferedGroup();
    Check(!phases.empty() && phases.back().index==MaxGenerated && phases.back().presentationTime==step && !phases.back().legacyRoute.retired);
    std::cout << checks << " assertions passed\n";
}
