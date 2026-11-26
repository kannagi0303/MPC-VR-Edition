// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cmath>
#include <iostream>

namespace {
using std::uint64_t; using std::int64_t;
static uint64_t clockTick = 100;
uint64_t GetTickCount64() { return clockTick; }
struct CDepthGpu { struct Timings {}; };
struct Owner {

#include "DepthCounterMethods.generated.inc"

    Counters m_counters;
    uint64_t m_mapRateTick=0, m_mapRateLastPoll=0, m_mapRateBase=0;
    void Poll(uint64_t now) {

#include "DepthPublicationRate.generated.inc"

    }
};
void Exercise() {
    Owner o;
    auto& c=o.m_counters;
    o.Poll(100);
    assert(!c.mapRateSampleTick);
    for (unsigned i=1;i<=25;i++) {
        ++c.maps; o.Poll(100+i*40);
    }
    assert(c.mapsPerSecond==25 && c.mapRateSampleTick==1100);
    // A busy or unavailable model contributes zero maps, not an invented FPS.
    for (unsigned i=1;i<=25;i++) o.Poll(1100+i*40);
    assert(c.mapsPerSecond==0 && c.mapRateSampleTick==2100);
    // Pause/resume: no averaging a long idle gap into the next active sample.
    ++c.maps; o.Poll(9000);
    assert(!c.mapRateSampleTick);
    for (unsigned i=1;i<=20;i++) { ++c.maps; o.Poll(9000+i*50); }
    assert(c.mapsPerSecond==20 && c.mapRateSampleTick==10000);
    for (unsigned i=0;i<10000;i++) {
        clockTick++;
        o.RecordSrEvaluation(i%4!=0);
        o.RecordMfgSubmission(i%5==0);
    }
    assert(c.srDepthRequests==10000 && c.srEvaluations==7500);
    assert(c.mfgDepthRequests==10000 && c.mfgSubmissions==2000);
    auto srTick=c.lastSrUseTick, mfgTick=c.lastMfgUseTick;
    clockTick++;
    o.RecordSrEvaluation(false); o.RecordMfgSubmission(false);
    assert(c.lastSrUseTick==srTick && c.lastMfgUseTick==mfgTick);
    assert(c.srDepthRequests==10001 && c.srEvaluations==7500);
    assert(c.mfgDepthRequests==10001 && c.mfgSubmissions==2000);
    std::cout<<"Depth playback statistics PASS (rate, idle gap, zero production, 20002 consumer requests)\n";
}
}

void TestDepthPlaybackStatistics() { Exercise(); }
