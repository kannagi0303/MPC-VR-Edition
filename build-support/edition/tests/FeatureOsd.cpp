// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/FeatureOsd.h"
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <limits>

void TestFeatureOsd() {
    using namespace edition::osd;
    const Request all{2,4,7,Color|Tone|Memory|Skin|Flat};
    Result sr{{1280,720},{3840,2160}}, fps{};
    SetCadence(fps,30000.0/1001,60,2);
    assert(fps.fromFps==30 && fps.toFps==60);
    assert(MainText(sr)==L"1280x720 -> 3840x2160");
    assert(MainText(fps)==L"30fps -> 60fps");
    Result combined=sr;combined.fromFps=30;combined.toFps=60;
    assert(MainText(combined)==L"1280x720 -> 3840x2160\n30fps    -> 60fps");
    assert(MainText({{3840,2160},{1980,1114}}).empty());
    assert(MainText({{3840,2160},{3840,2160}}).empty());
    assert(MainText({{0,0},{3840,2160}}).empty());
    for(unsigned mask=0;mask<32;++mask) {
        const auto s=IndicatorText(mask);
        assert((s.find(L"COLOR")!=s.npos)==bool(mask&Color));
        assert((s.find(L"TONE")!=s.npos)==bool(mask&Tone));
        assert((s.find(L"MEMORY")!=s.npos)==bool(mask&Memory));
        assert((s.find(L"SKIN")!=s.npos)==bool(mask&Skin));
        assert((s.find(L"2D")!=s.npos)==bool(mask&Flat));
        assert(s.empty()==!mask);
    }
    Result r{};SetCadence(r,24000.0/1001,60,3);assert(r.fromFps==24 && r.toFps==60);
    r={};SetCadence(r,60,60,2);assert(r.Empty());
    SetCadence(r,30,60,1);assert(r.Empty());
    SetCadence(r,std::numeric_limits<double>::infinity(),60,2);assert(r.Empty());
    Notice n;
    n.Observe(all,sr,100);
    assert(n.Visible(100).Empty());
    n.Observe(all,fps,600);
    assert(n.Visible(699).Empty());
    Result color{};color.flags=Color|Memory;
    n.Observe(all,color,700);
    n.Observe(all,{},1299);assert(n.Visible(1299).Empty());
    n.Observe(all,{},1300);assert(n.Visible(1300).flags==(Color|Memory));
    // No display timer until a successful draw. An unavailable tiny window may delay it.
    assert(n.Visible(10000).toFps==60);
    n.Drawn(10000);n.Drawn(10100);
    assert(!n.Visible(14999).Empty() && n.Visible(15000).Empty());
    // Repeated cache loss, resize observations, seek and steady results cannot replay it.
    for(uint64_t t=15000;t<25000;t+=17) {
        Result changed=sr;changed.output={2560,1440};
        n.Observe(all,t%2?changed:Result{},t);assert(n.Visible(t).Empty());
    }
    // A genuinely late effect updates the result group once, not on each draw.
    Result tone{};tone.flags=Tone;n.Observe(all,tone,25000);
    n.Observe(all,{},25599);assert(n.Visible(25599).Empty());
    n.Observe(all,{},25600);assert(n.Visible(25600).flags==(Color|Memory|Tone));
    n.Drawn(25600);n.Suppress();assert(n.Visible(25601).Empty());
    n.Observe(all,tone,26200);assert(n.Visible(26200).Empty());
    // Explicit disable removes stale result immediately; reenable requires fresh evidence.
    Request off=all;off.mfg=0;off.flags&=~Tone;
    n.Observe(off,fps,27000);assert(n.Visible(27000).Empty());
    n.Observe(all,fps,27100);n.Observe(all,{},27700);assert(n.Visible(27700).toFps==60);
    assert(!(n.Visible(27700).flags&Tone));
    n.Reset();assert(n.Visible(27701).Empty());
    // Target changes are explicit request changes; no measured FPS is needed.
    Request planned=all;planned.targetFromFps=30;planned.targetToFps=60;
    n.Observe(planned,fps,30000);n.Observe(planned,fps,30600);
    assert(n.Visible(30600).toFps==60);n.Drawn(30600);
    for(uint64_t t=30601;t<37000;t+=17) n.Observe(planned,t%2?fps:Result{},t);
    assert(n.Visible(37000).Empty());
    Result higher{};SetCadence(higher,30,120,4);
    planned.targetToFps=120;n.Observe(planned,higher,37000);
    assert(n.Visible(37599).Empty());n.Observe(planned,higher,37600);
    assert(n.Visible(37600).toFps==120);n.Drawn(37600);
    planned.targetFromFps=planned.targetToFps=0;n.Observe(planned,{},37700);
    assert(n.Visible(37700).Empty());
    unsigned cases=0;
    for(double source:{23.976,24.,25.,29.97,30.,50.,60.}) for(int target:{60,90,120,144}) {
        Result c{};SetCadence(c,source,target,6);
        if(c.Empty()) continue;
        Request req{};req.mfg=7;req.targetFromFps=c.fromFps;req.targetToFps=c.toFps;
        Notice notice;notice.Observe(req,c,1);notice.Observe(req,c,600);assert(notice.Visible(600).Empty());
        notice.Observe(req,c,601);assert(notice.Visible(601)==c);++cases;
    }
    std::cout<<"PASS FeatureOsd: format, 32 masks, lifecycle, target changes and "<<cases<<" planned cadence combinations\n";
}
#ifdef FEATURE_OSD_STANDALONE
int main(){TestFeatureOsd();}
#endif
