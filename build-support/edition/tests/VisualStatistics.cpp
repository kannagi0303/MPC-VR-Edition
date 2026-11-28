// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/VisualStatistics.h"
#undef NDEBUG
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

namespace {
using namespace edition::visual;
struct Rect { int l,y,r,b; uint32_t color; };
std::vector<Rect> Draw(const Timeline& t, int width=500) {
    std::vector<Rect> result;
    Emit(t,10,20,10+width,20,[&](int l,int y,int r,int b,uint32_t c){result.push_back({l,y,r,b,c});});
    for(const auto& r:result) assert(r.l>=10 && r.r<=10+width && r.l<r.r && r.y>=20 && r.b<=100 && r.y<r.b);
    return result;
}
void Exercise() {
    constexpr int64_t limit=20000000, head=50000000;
    const PixelRect old{10,20,30,40,RetainedColor};
    PixelRect next{11,20,29,40,RetainedColor};HoldPixelJitter(next,old);assert(next==old);
    next={12,20,29,40,RetainedColor};HoldPixelJitter(next,old);assert(next.left==12);
    next={11,20,29,40,PreparedColor};HoldPixelJitter(next,old);assert(next.left==11);
    next={11,20,29,40,0xFFFFFFFF};HoldPixelJitter(next,{10,20,30,40,0xFFFFFFFF});assert(next.left==11);
    assert(Fit(L"x",4)==L"x   " && Fit(L"x",4,true)==L"   x");
    assert(Fit(L"long",3)==L"lo~" && Fit(L"long",0).empty());
    const auto row=PairRow(L"Depth",L"30.0/s 3.42 ms",L"DLSS SR",L"3.46 ms");
    assert(row.size()==61 && row.substr(32,7)==L"DLSS SR" && row.substr(54)==L"3.46 ms" && row.find(L'~')==std::wstring::npos);
    assert(PairRow(L"",L"",L"",L"").empty());
    assert(PairRow(L"",L"",L"DLSS SR",L"Size limit").substr(0,32)==std::wstring(32,L' '));
    const auto hardware=HardwareRow({L"100%",L"n/a",L"0%",L"100%"});
    assert(hardware.size()==61 && hardware.substr(8,4)==L"100%" && hardware.substr(57)==L"100%");
    assert(hardware.find(L"Optical Flow 100%")!=std::wstring::npos && hardware.find(L'~')==std::wstring::npos);
    // Every effective-strength combination, including master-off and max values.
    constexpr const wchar_t* names[]={L"Color Enhancement",L"Memory Color",L"Adaptive Tone",L"Skin Tone"};
    for (unsigned mask=0; mask<16; ++mask) {
        std::array<int,4> strengths{};
        for (size_t j=0;j<4;++j) strengths[j]=(mask & (1u<<j)) ? (j==0 ? 100 : 50) : 0;
        const auto effects=EnhancementRows(strengths);
        assert(effects.empty()==(mask==0));
        assert(effects.find(L'~')==std::wstring::npos && effects.find(L"Active")==std::wstring::npos);
        for (size_t j=0;j<4;++j) assert((effects.find(names[j])!=std::wstring::npos)==(strengths[j]>0));
        for (size_t pos=0;pos<effects.size();pos+=62) assert(effects[pos]==L'\n' && effects.size()-pos>=62);
        if (mask & 1u) assert(effects.substr(26,4)==L"100%");
    }
    const auto native=VideoFormatRow(3840,2160,0,0,0,0,L"23.98 fps",false,L"60 Hz");
    assert(native.size()==61 && native.find(L"3840x2160  23.98 fps | SDR")==0 && native.find(L"->")==std::wstring::npos && native.substr(56)==L"60 Hz");
    const auto upscale=VideoFormatRow(1280,720,1280,720,3840,2160,L"29.97 fps",false,L"60 Hz");
    assert(upscale.size()==61 && upscale.find(L"1280x720 -> 3840x2160")==0);
    assert(VideoFormatRow(3840,2160,3840,2160,1989,1114,L"23.98 fps",false,L"60 Hz").find(L"->")==std::wstring::npos);
    assert(VideoFormatRow(3840,2160,1280,720,3840,2160,L"23.98 fps",false,L"60 Hz").find(L"->")==std::wstring::npos);
    assert(VideoFormatRow(3840,2160,3840,2160,3840,2160,L"23.98 fps",true,L"59.94 Hz").find(L"->")==std::wstring::npos);
    const auto axis=Axis();assert(axis.size()==61 && axis.substr(10,3)==L"Now" && axis.substr(58)==L"+2s");
    std::wstring text=L"native\nline";const auto editionStart=text.size();
    text+=L"\nEdition\nVideo Buffer\n"+axis;
    const auto track=FirstTrackRow(text);text+=L"\nVideo\nDepth\nLUT\nSR Input";
    text.insert(editionStart,1,L'\n');text.insert(editionStart+1,L"-----");
    int line=0;size_t start=0;for(size_t i=0;i<text.size();++i)if(text[i]==L'\n'){if(line==track)assert(text.substr(start,i-start)==L"Video");++line;start=i+1;}
    assert(track==6);
    Timeline t; t.limit=limit; t.visible={true,true,true,true};
    assert(Draw(t).empty());
    t.sampled=true;t.ready=19400000;t.queued=19800000;
    // Unsorted samples, out-of-window, negative PTS, overlap, adjacency and a real gap.
    t.AddSample(1,head+16300000,head,1000000);
    t.AddSample(1,head+15300000,head,1000000);
    t.AddSample(1,head+18300000,head,1000000);
    t.AddSample(1,head+limit,head,1000000);
    t.AddSample(1,-1,head,1000000);
    t.AddSample(2,head-100000,head,200000);
    t.AddSample(3,head+limit-100000,head,200000);
    for(auto& row:t.rows)row.Merge();
    assert(t.rows[1].count==2 && t.rows[1].data[0].begin==15300000 && t.rows[1].data[0].end==17300000);
    assert(t.rows[2].data[0].begin==0 && t.rows[2].data[0].end==100000);
    assert(t.rows[3].data[0].end==limit);
    t.AddCut(head+7600000,head);t.AddCut(head+7600000,head);t.AddCut(head-1,head);t.AddCut(head+limit,head);
    assert(t.cutCount==1 && t.cuts[0]==7600000);
    const auto rects=Draw(t);
    assert(rects.size()==16); // 4 backgrounds, 2 video spans, 3 neutral ready contexts, 4 cache spans, 1 cut, 2 boundaries
    assert(rects[1].r==505 && rects[2].r==495);
    assert(rects[13].color==0xFFFFFFFF && rects[13].l==200);
    // Project the same retained media sample from the current playhead. A
    // text statistics refresh must not change its position for the same head.
    Timeline live; live.limit=limit; live.sampled=true; live.visible={true,true,false,false};
    live.ready=t.ready; live.queued=t.queued;
    const auto position=[&](int64_t current) {
        live.rows[1].count=0; live.cutCount=0;
        live.AddSample(1,head+16300000,current,1000000);
        live.AddCut(head+7600000,current);
        return Draw(live);
    };
    const auto before=position(head+1000000), after=position(head+2000000);
    const auto findColor=[](const auto& v,uint32_t c,int y=-1){return std::find_if(v.begin(),v.end(),[&](const Rect& r){return r.color==c && (y<0 || r.y==y);});};
    assert(findColor(before,RetainedColor,44)->l-findColor(after,RetainedColor,44)->l==25);
    assert(findColor(before,0xFFFFFFFF)->l-findColor(after,0xFFFFFFFF)->l==25);
    const auto same=position(head+2000000);
    assert(same.size()==after.size());
    for(size_t i=0;i<same.size();++i) assert(same[i].l==after[i].l && same[i].r==after[i].r && same[i].color==after[i].color);
    const auto expired=position(head+18000000);
    assert(findColor(expired,0xFFFFFFFF)==expired.end() && findColor(expired,RetainedColor,44)==expired.end());
    Timeline shifted;shifted.limit=limit;shifted.sampled=true;shifted.visible[0]=true;
    shifted.AddCut(head+7600000,head+2000000);assert(shifted.cuts[0]==5600000);
    shifted.AddCut(head+1000000,head+2000000);assert(shifted.cutCount==1);
    // Full lookahead capacity: never silently omit depth slots beyond the normal eight.
    Timeline all; all.sampled=true;all.limit=limit;all.visible[1]=true;
    for(size_t i=0;i<Ranges::MaxSamples;++i)all.AddSample(1,head+int64_t(i)*50000,head,50000);
    assert(all.rows[1].count==Ranges::MaxSamples);all.rows[1].Merge();assert(all.rows[1].count==1);
    assert(Draw(all).size()==4); // background, a single continuous cache bar, two boundaries
    // All edges of all active rows are filtered, not just the right edge.
    // Real source-rate stepping plus small transient cache holes.
    for (int64_t period : {416666, 417083, 400000, 333333, 333667, 166666}) for (int displayFps : {60, 120, 144}) {
        constexpr int64_t lead=17500000;
        Timeline steady;
        std::array<int64_t,3> minBegin{limit,limit,limit}, maxBegin{}, minEnd{limit,limit,limit},maxEnd{};
        std::array<Range,3> priorBands{};
        for (int i=0;i<displayFps*10;++i) {
            const int64_t now=head+int64_t(i)*10000000/displayFps;
            const int64_t absEnd=head+lead+((now-head)/period)*period;
            if (i%11==0) steady.BeginSnapshot();
            steady.limit=limit;steady.sampled=true;steady.visible={true,true,true,true};
            steady.ready=19400000;steady.queued=19800000;
            for(auto& r:steady.rows)r.count=0;
            steady.rows[1].Add(absEnd-now-8*period,absEnd-now,limit);
            steady.rows[2].Add(absEnd-now-5*period,absEnd-now-3*period,limit);
            steady.rows[2].Add(absEnd-now-3*period+(i%2?period:0),absEnd-now,limit);
            steady.rows[2].Merge();
            steady.rows[3].Add(absEnd-now-period,absEnd-now,limit);
            steady.cutCount=0;steady.AddCut(now+5000000,now);
            steady.UpdateVisual(now,period);
            const auto raster=Draw(steady);
            assert(findColor(raster,0xFFFFFFFF)->l==135);
            assert(findColor(raster,RetainedColor,24)->r==495);
            for(size_t j=0;j<3;++j) {
                const auto& visual=steady.visual.rows[j+1];
                assert(visual.count==1);
                const auto band=visual.data[0];
                assert(band.begin>=0 && band.end<=limit && band.end>band.begin);
                assert(std::abs(band.begin-steady.rows[j+1].data[0].begin)<=period);
                assert(std::abs(band.end-steady.rows[j+1].data[steady.rows[j+1].count-1].end)<=period);
                if(i>displayFps*2) {
                    assert(std::abs(band.begin-priorBands[j].begin)<=period/8);
                    assert(std::abs(band.end-priorBands[j].end)<=period/8);
                    minBegin[j]=std::min(minBegin[j],band.begin);maxBegin[j]=std::max(maxBegin[j],band.begin);
                    minEnd[j]=std::min(minEnd[j],band.end);maxEnd[j]=std::max(maxEnd[j],band.end);
                }
                priorBands[j]=band;
            }
        }
        for(size_t j=0;j<3;++j) {
            // Fractional source rates also exhibit a slow beat against the
            // display clock; the full-window span includes that drift.
            assert(maxBegin[j]-minBegin[j]<=period/3);
            assert(maxEnd[j]-minEnd[j]<=period/3);
        }
    }
    constexpr int64_t period=400000;
    Timeline filtered;filtered.sampled=true;filtered.limit=limit;filtered.visible={true,true,true,true};
    filtered.ready=19000000;filtered.queued=20000000;
    filtered.rows[1].Add(10000000,15000000,limit);
    filtered.UpdateVisual(head,period);
    filtered.rows[1].count=0;filtered.rows[1].Add(10200000,15200000,limit);
    filtered.ready=15000000;filtered.queued=16000000;
    filtered.UpdateVisual(head+period,period);
    assert(filtered.visual.rows[1].data[0].begin>10000000 && filtered.visual.rows[1].data[0].begin<10200000);
    assert(filtered.visual.rows[1].data[0].end>15000000 && filtered.visual.rows[1].data[0].end<15200000);
    assert(filtered.visual.ready>filtered.ready && filtered.visual.ready<19000000);
    assert(filtered.visual.queued>=filtered.visual.ready);
    const auto previous=filtered.visual.rows[1].data[0];
    filtered.UpdateVisual(head+period,period);
    assert(filtered.visual.rows[1].data[0].begin==previous.begin && filtered.visual.rows[1].data[0].end==previous.end);
    // Real large holes remain distinct; no ghost cache when the row is empty.
    filtered.rows[1].count=0;filtered.rows[1].Add(10000000,11000000,limit);filtered.rows[1].Add(14000000,15000000,limit);
    filtered.UpdateVisual(head+period*2,period);
    assert(filtered.visual.rows[1].count==2);
    assert(filtered.visual.rows[1].data[1].begin-filtered.visual.rows[1].data[0].end>=1500000);
    filtered.rows[1].count=0;filtered.UpdateVisual(head+period*3,period);
    assert(filtered.visual.rows[1].count==0);
    filtered.rows[1].Add(4000000,6000000,limit);filtered.UpdateVisual(head,period);
    assert(filtered.visual.rows[1].data[0].begin==4000000 && filtered.visual.ready==filtered.ready);
    filtered.ready=0;filtered.queued=0;filtered.UpdateVisual(head+period,period);
    assert(!filtered.visual.ready && !filtered.visual.queued);
    filtered={};assert(filtered.visual.head==-1 && !filtered.visual.rows[1].count);
    std::mt19937 rng(0xE501);
    VisualState previousVisual;
    for(unsigned i=0;i<20000;++i){
        Timeline a;a.limit=limit;a.sampled=true;a.visible={true,bool(rng()%2),bool(rng()%2),bool(rng()%2)};
        a.ready=rng()%limit;a.queued=a.ready+rng()%limit;
        // Compare exact interval union against a brute-force time grid.
        bool covered[100]{};
        const auto step=limit/100;
        for(unsigned j=0;j<64;++j){int first=int(rng()%130)-15;int end=first+int(rng()%12);
            a.rows[1].Add(first*step,end*step,limit);
            for(int k=(std::max)(0,first);k<(std::min)(100,end);++k)covered[k]=true;
        }
        a.rows[1].Merge();
        for(int k=0;k<100;++k){bool found=false;for(size_t j=0;j<a.rows[1].count;++j){const auto r=a.rows[1].data[j];found|=r.begin<=k*step && (k+1)*step<=r.end;}assert(found==covered[k]);}
        for(unsigned j=0;j<8;++j)a.AddCut(head+int64_t(rng()%limit),head);
        Draw(a,1+int(rng()%1200));
        a.visual=previousVisual;
        a.UpdateVisual(head+int64_t(i)*166667,period);
        const auto& bands=a.visual.rows[1];
        for(size_t j=0;j<bands.count;++j){
            assert(bands.data[j].begin>=0 && bands.data[j].end<=limit && bands.data[j].begin<bands.data[j].end);
            if(j) assert(bands.data[j-1].end<=bands.data[j].begin);
        }
        Draw(a,1+int(rng()%1200));
        previousVisual=a.visual;
    }
    std::cout<<"PASS: 20,000 randomized timeline/rectangle cases; full 320-slot merge, scene clipping and scrolling\n";
}
}
void TestVisualStatistics(){Exercise();}
#ifdef VISUAL_STATISTICS_STANDALONE
int main(){TestVisualStatistics();}
#endif
