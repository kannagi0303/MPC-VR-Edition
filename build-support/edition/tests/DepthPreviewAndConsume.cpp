// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include "Depth/DepthRawPolicy.h"
#include <cassert>
#include <cstdint>
#include <string_view>
#include <iostream>
#include <random>

namespace {
using std::int64_t;
struct CDepth {
    static constexpr int64_t MaxEstimateAge = 2000000;
    struct Estimate { bool view=false; int64_t pts=-1; };
    Estimate map{false,-1}, ready{false,-1};
    int64_t scene=-1,m_playbackPts=-1,m_previousPlaybackPts=-1,m_selectedSourcePts=-1,m_selectedDepthPts=-1,m_originalDuration=400000;
    bool m_depthShader=true;
    struct Counters { int64_t rawMfgCheckAge=-1; const wchar_t* rawMfgCheck=L"not checked"; } m_counters;
    void PollCompleted() { if(ready.view) { map=ready; ready={false,-1}; } }
    Estimate EstimateAtOrBefore(int64_t pts) const { return map.view && map.pts<=pts ? map : Estimate{false,-1}; }
    int64_t SceneStart(int64_t) const { return scene; }
    Estimate SelectOriginal(int64_t pts); Estimate ForFrame(int64_t pts) const;
bool Check(int64_t pts,float frameTimeMs,bool inverted) {

#include "DepthConsumerGate.generated.inc"

    record(L"used");
    return true;
}
};
#include "DepthOriginalSelection.generated.inc"
void Exercise(){
    using std::wstring_view;
    CDepth a;
    a.ready={true,0};
    assert(a.Check(400000,40,true));
    assert(a.m_counters.rawMfgCheckAge==400000);
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"used");
    assert(!a.Check(800000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"PTS mismatch");
    a.scene=1;
    assert(!a.Check(400000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"scene mismatch");
    a.scene=-1;
    assert(!a.Check(3000000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"too old");
    a.m_depthShader=false;
    assert(!a.Check(400000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"shader unavailable");
    a.map={true,500000};
    assert(!a.Check(400000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"no map");
    assert(a.m_counters.rawMfgCheckAge==-1);
    // Current-original selection is pinned, not replaced by late completion.
    CDepth pin;pin.m_playbackPts=400000;pin.m_previousPlaybackPts=0;pin.map={true,0};
    assert(pin.Check(400000,40,true));pin.ready={true,400000};
    assert(!pin.Check(400000,40,true)); // mocked single-slot eviction of pinned map
    CDepth miss;miss.m_playbackPts=400000;miss.m_previousPlaybackPts=0;
    assert(!miss.Check(400000,40,true));miss.ready={true,400000};assert(!miss.Check(400000,40,true));
    CDepth interval;interval.m_playbackPts=800000;interval.m_previousPlaybackPts=400000;interval.map={true,600000};
    assert(!interval.Check(800000,40,true));
    std::mt19937 rng(26);
    for(int i=0;i<100000;++i){
        CDepth b;
        b.map={bool(rng()%2),int64_t(rng()%3000000)};
        b.scene=int64_t(rng()%3000000)-1;
        b.m_depthShader=bool(rng()%2);
        int64_t pts=rng()%3000000;
        float frameTimeMs=float(rng()%100);
        const auto e=b.EstimateAtOrBefore(pts);
        bool prior=e.view && e.pts>=b.scene && pts-e.pts<=CDepth::MaxEstimateAge && b.m_depthShader &&
            depthraw::Usable(e.pts,pts,frameTimeMs);
        assert(b.Check(pts,frameTimeMs,true)==prior);
    }
    std::cout<<"PASS: late harvest, RAW rejection reasons, 100000 equivalent eligibility cases\n";
}
}

void TestDepthPreviewAndConsume() { Exercise(); }
