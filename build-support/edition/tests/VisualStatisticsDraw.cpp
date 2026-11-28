// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/VisualStatistics.h"
#undef NDEBUG
#include <cassert>
#include <cwchar>
#include <iostream>
#include <vector>
namespace {
using LONG=long; using HRESULT=long; using D3DCOLOR=uint32_t;
struct SIZE{LONG cx=0,cy=0;}; struct POINT{LONG x,y;};
constexpr HRESULT S_OK=0; constexpr HRESULT E_FAIL=-1;
constexpr bool FAILED(HRESULT r){return r<0;}
struct Font {
    int reads=0;
    HRESULT GetTextExtent(const wchar_t* text,SIZE* size){++reads;int lines=1,cols=0,max=0;for(;*text;++text){if(*text==L'\n'){++lines;max=std::max(max,cols);cols=0;}else ++cols;} size->cx=10*std::max(max,cols);size->cy=20*lines;return S_OK;}
};
struct Geometry {
    int init=0,clear=0,uploads=0,draws=0,rects=0;bool ready=false,fail=false;
    bool Initialized() const{return ready;}
    HRESULT InitDeviceObjects(int,int){++init;ready=true;return S_OK;}
    void InvalidateDeviceObjects(){ready=false;}
    void ClearPoints(SIZE&){++clear;rects=0;}
    void Rect(int l,int y,int r,int b,D3DCOLOR){assert(l<r&&y<b);++rects;}
    HRESULT UpdateVertexBuffer(){++uploads;return fail?E_FAIL:S_OK;}
    void Draw(){assert(ready&&rects>0);++draws;}
};
struct Rect { LONG left=0,top=0,right=0,bottom=0; void SetRect(LONG l,LONG t,LONG r,LONG b){left=l;top=t;right=r;bottom=b;} LONG Width()const{return right-left;} };
using ULONGLONG=uint64_t;
constexpr int State_Running=1;
struct Filter {int m_filterState=State_Running; uint64_t m_completedEpoch=1; struct Flag { bool load()const{return true;} } m_videoEnhancement;};
struct CDepthGpu {static constexpr size_t Slots=320;};
struct Depth {
    std::array<int64_t,CDepthGpu::Slots> pts{};
    std::array<int64_t,8> cuts{};
    Depth(){pts.fill(-1);cuts.fill(-1);}
    const auto& CachedResultPts()const{return pts;}
    const auto& SceneCutPts()const{return cuts;}
};
struct LutFrame {bool view=false,parameters=false;uint64_t fence=0;int64_t pts=-1;};
struct Lut {std::array<LutFrame,16> frames{};bool heldView=false,heldParameters=false;uint64_t heldFence=0;int64_t heldPts=-1;};
struct EarlyOriginalImage {bool ready=false,srSource=false,consumed=false;uint64_t epoch=1;int64_t pts=-1;struct {bool pTexture=false;} rgb;};
constexpr int SUPERRES_DLSS_SR=1;
struct CDX11VideoProcessor {
    edition::visual::Timeline m_visualTimeline;
    Geometry m_visualGeometry;Font m_Font3D;
    bool m_visualGeometryDirty=true,m_visualGeometryValid=false;
    SIZE m_visualGeometrySize{}; POINT m_StatsTextPoint{15,15};LONG m_statsFeatureOffset=0;
    int m_pDevice=1,m_pDeviceContext=1;
    Rect m_visualLayout; LONG m_visualLine=0;
    const edition::visual::PresentClock* m_visualPresentClock=nullptr;
    std::vector<edition::visual::PixelRect> m_visualRaster,m_visualUploadedRaster;
    int64_t m_rtVideoDelay=1000000,m_rtAvgTimePerFrame=400000;
    Depth m_depth; Lut m_svdLutRawTest; EarlyOriginalImage m_earlyOriginal,m_futureOriginal;
    int m_iVPSuperRes=SUPERRES_DLSS_SR; bool m_dlssSRAfterMfg=false;
    bool UsesDepthSr()const{return true;} bool UsesDepthMfg()const{return true;}
    bool EarlyOriginalDomainMatches(const EarlyOriginalImage&)const{return true;}
    Filter filter; Filter* m_pFilter=&filter;
    void RefreshEditionTimelineResources(edition::visual::Timeline& t,int64_t head,uint64_t fence);
    void DrawEditionTimeline(const SIZE& rtSize);
};
#include "VisualStatisticsDraw.generated.inc"
} // namespace
void TestVisualStatisticsDraw(){
    using namespace edition::visual;
    CDX11VideoProcessor p; auto& t=p.m_visualTimeline;t.sampled=true;t.limit=20000000;t.ready=19000000;t.queued=20000000;t.visible={true,true,true,true};t.textRow=20;
    p.m_depth.pts[0]=18000000;p.m_depth.cuts[0]=15000000;
    p.m_svdLutRawTest.frames[0]={true,true,10,18500000};
    p.m_earlyOriginal={true,true,false,1,19000000,{true}};
    PresentClock clock{1000000,9};p.m_visualPresentClock=&clock;
    auto draw=[&](){p.DrawEditionTimeline({1920,1080});};
    const auto color=[&](uint32_t c,int row=-1){return std::find_if(p.m_visualUploadedRaster.begin(),p.m_visualUploadedRaster.end(),[&](const PixelRect& r){return r.color==c && (row<0 || r.top==p.m_visualLayout.top+row*p.m_visualLine+std::max(1L,p.m_visualLine/5));});};
    draw();assert(p.m_Font3D.reads==3 && p.m_visualGeometry.uploads==1 && p.m_visualGeometry.draws==1);
    assert(color(RetainedColor,2)==p.m_visualUploadedRaster.end());
    for(int i=0;i<10000;++i)draw();assert(p.m_Font3D.reads==3 && p.m_visualGeometry.uploads==1 && p.m_visualGeometry.draws==10001);
    auto x=color(RetainedColor,1)->left, cut=color(0xFFFFFFFF)->left;
    clock.stream+=166667;draw();assert(color(RetainedColor,1)->left<=x && color(0xFFFFFFFF)->left<cut);
    x=color(RetainedColor,1)->left;
    const auto depthBefore=t.visual.rows[1].data[0];
    clock.fence=10;draw();assert(color(RetainedColor,2)!=p.m_visualUploadedRaster.end());
    assert(t.visual.rows[1].data[0].begin==depthBefore.begin && t.visual.rows[1].data[0].end==depthBefore.end);
    // A topology change may release the one-pixel raster hold, but must
    // not change the filtered time coordinates of an unchanged sample.
    assert(std::abs(color(RetainedColor,1)->left-x)<=1);
    p.m_visualGeometryDirty=true;draw();assert(std::abs(color(RetainedColor,1)->left-x)<=1);
    x=color(RetainedColor,1)->left;
    p.filter.m_filterState=0;clock.stream+=2000000;const auto pauseUploads=p.m_visualGeometry.uploads;draw();assert(p.m_visualGeometry.uploads==pauseUploads);
    p.filter.m_filterState=State_Running;draw();assert(color(RetainedColor,1)->left<x);
    p.m_depth.pts[0]=-1;draw();assert(color(RetainedColor,1)==p.m_visualUploadedRaster.end());
    p.m_earlyOriginal.epoch=2;draw();assert(color(RetainedColor,3)==p.m_visualUploadedRaster.end());
    p.m_visualPresentClock=nullptr;const auto u=p.m_visualGeometry.uploads;draw();assert(p.m_visualGeometry.uploads==u);
    p.m_visualGeometryDirty=true;p.m_visualGeometry.fail=true;draw();assert(!p.m_visualGeometryValid && p.m_visualGeometry.uploads==u+1);
    for(int i=0;i<100;++i)draw();assert(p.m_visualGeometry.uploads==u+1);
    p.m_visualGeometryDirty=true;p.m_visualGeometry.fail=false;draw();assert(p.m_visualGeometryValid && p.m_visualGeometry.uploads==u+2);
    p.DrawEditionTimeline({400,300});assert(!p.m_visualGeometryValid && p.m_visualGeometry.uploads==u+2);
    draw();assert(p.m_visualGeometryValid && p.m_visualGeometry.uploads==u+3);
    t.sampled=false;const auto n=p.m_visualGeometry.draws;draw();assert(p.m_visualGeometry.draws==n);
    // Actual production draw entry: steady retained PTS frontier, 30fps
    // source and 60Hz draw, across ordinary statistics snapshot refreshes.
    CDX11VideoProcessor q;
    auto& st=q.m_visualTimeline;
    PresentClock pc{1000000,10};q.m_visualPresentClock=&pc;
    q.m_rtAvgTimePerFrame=333333;
    const int64_t period=q.m_rtAvgTimePerFrame;
    int minRight=INT32_MAX,maxRight=0,minLeft=INT32_MAX,maxLeft=0;
    for(int i=0;i<600;++i){
        const int64_t head=int64_t(i)*10000000/60;
        const int64_t last=16000000+(head/period)*period;
        if(i%15==0){st.BeginSnapshot();q.m_visualGeometryDirty=true;}
        st.sampled=true;st.limit=20000000;st.ready=19000000;st.queued=20000000;
        st.visible={true,true,false,false};st.textRow=20;
        for(int j=0;j<8;++j)q.m_depth.pts[j]=last-period*(7-j);
        pc.stream=head+q.m_rtVideoDelay;
        q.DrawEditionTimeline({1920,1080});
        const auto band=std::find_if(q.m_visualUploadedRaster.begin(),q.m_visualUploadedRaster.end(),[&](const PixelRect& r){return r.color==RetainedColor && r.top==q.m_visualLayout.top+q.m_visualLine+std::max(1L,q.m_visualLine/5);});
        assert(band!=q.m_visualUploadedRaster.end());
        if(i>120){minRight=std::min(minRight,band->right);maxRight=std::max(maxRight,band->right);minLeft=std::min(minLeft,band->left);maxLeft=std::max(maxLeft,band->left);}
    }
    assert(maxRight-minRight<=2 && maxLeft-minLeft<=2);
    // No retained resource: the colored band must vanish on the next draw.
    q.m_depth.pts.fill(-1);q.DrawEditionTimeline({1920,1080});
    assert(std::none_of(q.m_visualUploadedRaster.begin(),q.m_visualUploadedRaster.end(),[&](const PixelRect& r){return r.color==RetainedColor && r.top==q.m_visualLayout.top+q.m_visualLine+std::max(1L,q.m_visualLine/5);}));
    std::cout<<"PASS: actual draw/resource methods, 10,000 identical Presents; true clock, fence gating, eviction, epoch, pause, resize, retry\n";
}
