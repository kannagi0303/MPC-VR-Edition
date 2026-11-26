// Portable production queue-policy tests. No GPU/driver timing claims.
#include "../../../Source/Depth/DepthJobQueue.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <random>
#include <set>
struct Result { unsigned slot = 0; std::uint64_t pts = 0; };
using Queue = depthjobs::Queue<Result>;
static void Check(const Queue& q) {
    std::set<unsigned> slots;
    auto add = [&](unsigned slot) { assert(slots.insert(slot).second); assert(q.SlotBusy(slot)); };
    if (q.pending) add(q.pendingJob.frame.slot);
    if (q.running) add(q.runningJob.frame.slot);
    if (q.pending && q.running) assert(q.pendingJob.captureBank != q.runningJob.captureBank);
    for (unsigned i=0;i<q.count;++i) add(q.results[i].slot);
    assert(slots.size() <= Queue::MaxOutstanding);
    for(unsigned i=0;i<8;++i) assert(q.SlotBusy(i)==slots.contains(i));
}
static unsigned Simulate(unsigned jobUs) {
    Queue q;
    unsigned submitted=0, harvested=0, finish=0;
    // Idealized producer at 23.976fps, instant capture/harvest, fixed job
    // duration. This tests cadence quantization, NOT GPU performance.
    for(unsigned t=0;t<60'000'000;t+=100) {
        if(q.running && t>=finish) q.Complete(q.runningJob.frame);
        if(t%41'700==0) {
            Result r; while(q.Take(r)) ++harvested;
            if(q.CanAccept()) {
                unsigned slot=0; while(q.SlotBusy(slot)) ++slot;
                const auto bank=q.CaptureBank();
                assert(q.CanSubmit(bank,slot));
                q.Submit({{slot,++submitted},submitted,bank});
            }
        }
        if(q.pending && !q.running) {q.Begin(); finish=t+jobUs;}
    }
    return harvested;
}
int main() {
    std::mt19937 rng(26);
     {
        Queue q; std::uint64_t serial=0, last=0;
        for(unsigned n=0;n<200000;++n) {
            switch(rng()%5) {
            case 0: if(q.CanAccept()) {
                unsigned bank=q.CaptureBank(), slot=0; while(q.SlotBusy(slot)) ++slot;
                // Producer may select a capture bank just before GPU completion.
                if(q.running && rng()%2) q.Complete(q.runningJob.frame);
                assert(q.CanSubmit(bank,slot));
                q.Submit({{slot,++serial},serial,bank});
                assert(!q.CanAccept());
                break;
            } break;
            case 1: if(q.pending && !q.running) q.Begin(); break;
            case 2: if(q.running) q.Complete(q.runningJob.frame); break;
            case 3: { Result r; if(q.Take(r)) {assert(r.pts>last); last=r.pts;} break; }
            case 4: q.CancelPending(); break;
            }
            Check(q);
        }
    }
    const auto pipelined=Simulate(46'000);
    assert(pipelined>=1290 && pipelined<=1310);
    assert(Simulate(14'000)>=1435);
    std::cout << "PASS: 200000 queue transitions; fixed 46ms simulation " << pipelined/60.0 << " maps/s (not GPU measurement)\n";
}
