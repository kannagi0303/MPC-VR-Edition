// SPDX-License-Identifier: GPL-3.0-or-later
// Actual DX11 corner notice methods, with CPU-only renderer stand-ins.
#include "../../../Source/Edition/FeatureOsd.h"
#include "../../../Source/Mfg/AutoCadence.h"
#include "../../../Source/Mfg/DisplayCadence.h"
#undef NDEBUG
#include <cassert>
#include <atomic>
#include <mutex>
#include <vector>
#include <iostream>
namespace {
using LONG=long;using D3DCOLOR=uint32_t;using HRESULT=long;using UINT=unsigned;using ULONGLONG=uint64_t;
constexpr HRESULT S_OK=0,S_FALSE=1,E_FAIL=-1;
constexpr bool SUCCEEDED(HRESULT h){return h>=0;} constexpr bool FAILED(HRESULT h){return h<0;}
constexpr int State_Running=1,State_Paused=2,SUPERRES_Disable=0,SUPERRES_DLSS_SR=6,PCIV_NVIDIA=0x10de;
constexpr int DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709=0;
constexpr int64_t INVALID_TIME=-1,UNITS=10000000;
uint64_t tick=0; uint64_t GetTickCount64(){return tick;}
constexpr uint32_t D3DCOLOR_ARGB(unsigned a,unsigned r,unsigned g,unsigned b){return (a<<24)|(r<<16)|(g<<8)|b;}
constexpr uint32_t D3DCOLOR_XRGB(unsigned r,unsigned g,unsigned b){return D3DCOLOR_ARGB(255,r,g,b);}
struct SIZE{LONG cx=0,cy=0;};struct CSize:SIZE{CSize(LONG w,LONG h):SIZE{w,h}{}};
struct RECT{LONG left,top,right,bottom;};struct Rect{LONG Width()const{return 1280;} LONG Height()const{return 720;}};
struct D3D11_TEXTURE2D_DESC{UINT Width=0,Height=0;};struct ID3D11Texture2D {UINT w=1280,h=720;void GetDesc(D3D11_TEXTURE2D_DESC* d){*d={w,h};}};
struct ID3D11RenderTargetView{};
template<class T>struct CComPtr {T* p=nullptr;T** operator&(){return &p;}operator T*()const{return p;}};
struct Device{ID3D11RenderTargetView view; HRESULT CreateRenderTargetView(ID3D11Texture2D*,void*,ID3D11RenderTargetView** v){*v=&view;return S_OK;}};
struct Font {
    std::vector<std::wstring> text;std::vector<uint32_t> colors;std::vector<std::pair<LONG,LONG>> coords;bool fail=false,initFail=false;int height=14,inits=0,invalidates=0,atlases=0;
    HRESULT InitDeviceObjects(Device*,void*){++inits;return initFail?E_FAIL:S_OK;}
    void InvalidateDeviceObjects(){++invalidates;}
    HRESULT CreateFontBitmap(const wchar_t*,int h,int){if(height!=h){++atlases;height=h;}return S_OK;}
    HRESULT GetTextExtent(const wchar_t* text,SIZE* size){LONG line=0,max=0,lines=1;for(;*text;++text){if(*text==L'\n'){max=std::max(max,line);line=0;++lines;}else ++line;}*size={(height*6/10)*std::max(max,line),height*lines};return S_OK;}
    HRESULT Draw2DText(ID3D11RenderTargetView*,SIZE size,LONG x,LONG y,uint32_t color,const wchar_t* s){if(fail)return E_FAIL;SIZE extent;GetTextExtent(s,&extent);assert(x>=0&&y>=0&&x+extent.cx<=size.cx&&y+extent.cy<=size.cy);text.emplace_back(s);colors.push_back(color);coords.emplace_back(x,y);return S_OK;}
};
struct Background {int count=0,inits=0,invalidates=0;RECT rect{};uint32_t color=0;bool fail=false;
    HRESULT InitDeviceObjects(Device*,void*){++inits;return S_OK;}void InvalidateDeviceObjects(){++invalidates;}
    HRESULT Set(RECT r,SIZE s,uint32_t c){assert(r.left>=0&&r.top>=0&&r.right<=s.cx&&r.bottom<=s.cy);rect=r;color=c;return fail?E_FAIL:S_OK;}
    HRESULT Draw(ID3D11RenderTargetView*,SIZE){++count;return S_OK;}};
struct Filter {
    int m_filterState=State_Running;bool m_bFlushing=false;
    std::atomic<unsigned> m_featureOsdRun{1};
    std::atomic<int> m_videoEnhancement{0},m_adaptiveTone{0},m_memoryColor{0},m_skinTone{0};
};
struct SR {struct Stats{UINT inputWidth=1280,inputHeight=720,outputWidth=3840,outputHeight=2160;};Stats stats;Stats GetStats(){return stats;}};
struct Mfg {bool fail=false;unsigned cap=4;bool Failed()const{return fail;}unsigned MaxAvailableMultiplier()const{return cap;}};
struct CDX11VideoProcessor {
    Device device;Device* m_pDevice=&device;void* m_pDeviceContext=&device;Filter filter;Filter* m_pFilter=&filter;
    std::atomic<bool> m_showEditionOsd{true};bool m_bShowStats=false;
    std::mutex m_featureStatusMutex;edition::osd::Notice m_featureNotice;
    uint64_t m_featureStatusLastPerceptual=0;unsigned m_featureStatusOsdRun=0;bool m_featureProjectionReady=false;
    std::atomic<uint64_t> m_mfgGeneratedPresents{0},m_mfgRealPresents{0},m_mfgFallbackPresents{0};
    std::atomic<unsigned> m_mfgMultiplier{0},m_mfgEffectiveMultiplier{2};std::atomic<bool> m_mfgPresentFailed{false};
    struct {uint64_t draws=0;}m_perceptual;struct {bool failed=false;int64_t activationPts=INVALID_TIME;}m_svdLutRawTest;
    struct {bool shaderFailed=false,applied=false;}m_adaptiveTone;
    int m_iVPSuperRes=SUPERRES_DLSS_SR,m_dlssSRSourceLimit=4,m_VendorId=PCIV_NVIDIA,m_currentSwapChainColorSpace=0;
    bool m_bVPUseRTXVideoHDR=false,m_pPSHalfOUtoInterlace=false,m_jvrFlatView=false,m_bVPUseSuperRes=false,m_jvrShader=false;
    struct{bool pTexture=false;}m_jvrFrame;
    const wchar_t* m_dlssSRInputStatus=nullptr;SR m_dlssSR;Rect m_srcRect;
    struct{SIZE input{},output{};uint64_t tick=0;}m_editionVsr;
    Mfg m_mfg;int64_t m_rtAvgTimePerFrame=333667;double m_dRefreshPeriodMs=1000.0/60;
    bool bypass=false;bool SourceIsHDR(){return false;}bool CanProjectJvr(){return m_jvrFlatView;}bool MfgSourceMeetsDisplayRate(){return bypass;}
    int m_StatsFontH=14;Font m_Font3D,m_featureOsdFont;Background m_featureOsdBackground;HRESULT m_featureOsdResourceStatus=S_FALSE;
    void ResetFeatureStatus();LONG DrawFeatureStatus(ID3D11Texture2D*);
};
#include "FeatureOsdDraw.generated.inc"
void Exercise() {
    CDX11VideoProcessor p;ID3D11Texture2D target;
    auto at=[&](uint64_t now){tick=now;return p.DrawFeatureStatus(&target);};
    p.filter.m_memoryColor=50;p.filter.m_adaptiveTone=50;
    assert(!at(1));p.m_perceptual.draws=1;p.m_adaptiveTone.applied=true;
    assert(!at(17));assert(!at(616));
    target.w=32;assert(!at(617));target.w=1280;
    assert(p.m_featureOsdFont.inits==0 && p.m_featureOsdBackground.inits==0);
    assert(at(800)>0);assert(p.m_featureOsdFont.text.size()==2);
    assert(p.m_featureOsdFont.text[0]==L"1280x720 -> 3840x2160" && p.m_featureOsdFont.text[1]==L"TONE  MEMORY");
    assert(p.m_featureOsdFont.colors[0]==0xffffffff && p.m_featureOsdFont.colors[1]==0xff75d6a4);
    assert(p.m_featureOsdBackground.color==0xcc000000 && p.m_featureOsdBackground.count==1);
    assert(p.m_featureOsdFont.height==18 && p.m_featureOsdFont.inits==1 && p.m_featureOsdBackground.inits==1);
    assert(p.m_Font3D.text.empty() && p.m_Font3D.inits==0 && p.m_Font3D.height==14);
    for(size_t i=0;i<2;++i){
        auto [x,y]=p.m_featureOsdFont.coords[i];SIZE extent;
        p.m_featureOsdFont.GetTextExtent(p.m_featureOsdFont.text[i].c_str(),&extent);
        auto r=p.m_featureOsdBackground.rect;
        assert(x>r.left && y>r.top && x+extent.cx<r.right && y+extent.cy<r.bottom);
    }
    p.filter.m_filterState=State_Paused;assert(!at(1000));
    p.filter.m_filterState=State_Running;assert(at(1500)>0);
    assert(!at(5800));p.filter.m_bFlushing=true;assert(!at(5900));p.filter.m_bFlushing=false;
    p.m_dlssSR.stats.outputWidth=2560;p.m_dlssSR.stats.outputHeight=1440;assert(!at(6000));
    p.filter.m_skinTone=50;++p.m_perceptual.draws;assert(!at(6100));p.m_bShowStats=true;assert(!at(6700));
    p.m_bShowStats=false;assert(!at(6710)); // no replay after Stats
    p.m_showEditionOsd=false;assert(!at(7000));
    p.ResetFeatureStatus();p.m_showEditionOsd=true;++p.filter.m_featureOsdRun;
    assert(!at(8000));++p.m_perceptual.draws;assert(!at(8017));assert(at(8617)>0);
    // FPS-only result with the actual AutoDisplaySlots policy. The newest
    // producer multiplier changes, but the nominal target stays constant.
    // No SR output or enhancement flags may accidentally trigger this notice.
    for (int source : {24,25,30,50}) {
        CDX11VideoProcessor f;f.m_iVPSuperRes=SUPERRES_Disable;
        f.m_mfgMultiplier=mfg::AutoMultiplier;f.m_rtAvgTimePerFrame=UNITS/source;
        bool shown=false;uint64_t first=0;
        for (int frame=0;frame<480;++frame) {
            tick=10000+uint64_t(frame)*1000/60;
            const int src=frame*source/60;
            const auto pts=int64_t(src+1)*f.m_rtAvgTimePerFrame;
            f.m_mfgEffectiveMultiplier=mfg::AutoDisplaySlots(pts-f.m_rtAvgTimePerFrame,pts,0,60);
            f.m_mfgRealPresents=src;f.m_mfgGeneratedPresents=frame-src;
            if (f.DrawFeatureStatus(&target)>0) {
                if (!shown) first=tick;
                shown=true;assert(f.m_featureOsdFont.text.back()==std::to_wstring(source)+L"fps -> 60fps");
            }
        }
        assert(shown && first==10600);
        assert(!f.DrawFeatureStatus(&target)); // not replayed after expiry
    }
    // Capability-limited Auto must not promise an unreachable 60fps.
    // Manual mode retains its own actual effective multiplier.
    for (unsigned request : {mfg::AutoMultiplier,2u}) {
        CDX11VideoProcessor f;f.m_iVPSuperRes=SUPERRES_Disable;
        f.m_mfgMultiplier=request;f.m_mfg.cap=2;f.m_mfgEffectiveMultiplier=2;
        f.m_rtAvgTimePerFrame=UNITS/24;bool shown=false;
        for (int frame=0;frame<96;++frame) {
            tick=20000+uint64_t(frame)*1000/48;
            f.m_mfgRealPresents=frame/2;f.m_mfgGeneratedPresents=frame-frame/2;
            if(f.DrawFeatureStatus(&target)>0) {shown=true;assert(f.m_featureOsdFont.text.back()==L"24fps -> 48fps");}
        }
        assert(shown);
    }
    // No generated or successful Present count at all: planned FPS still appears.
    {CDX11VideoProcessor f;f.m_iVPSuperRes=SUPERRES_Disable;f.m_mfgMultiplier=mfg::AutoMultiplier;
        f.m_mfgEffectiveMultiplier=0;tick=30000;assert(!f.DrawFeatureStatus(&target));
        tick=30600;assert(f.DrawFeatureStatus(&target)>0);assert(f.m_featureOsdFont.text.back()==L"30fps -> 60fps");
        tick=35600;assert(!f.DrawFeatureStatus(&target));
        f.m_mfg.fail=true;tick=35700;assert(!f.DrawFeatureStatus(&target));
        f.m_mfg.fail=false;tick=36400;assert(!f.DrawFeatureStatus(&target));
        f.m_dRefreshPeriodMs=1000.0/120;tick=37000;assert(!f.DrawFeatureStatus(&target));
        tick=37600;assert(f.DrawFeatureStatus(&target)>0);assert(f.m_featureOsdFont.text.back()==L"30fps -> 120fps");
    }
    // Manual target uses configured budget, not transient effective multiplier.
    {CDX11VideoProcessor f;f.m_iVPSuperRes=SUPERRES_Disable;f.m_mfgMultiplier=3;f.m_mfgEffectiveMultiplier=0;f.m_rtAvgTimePerFrame=UNITS/24;
        tick=40000;assert(!f.DrawFeatureStatus(&target));tick=40600;assert(f.DrawFeatureStatus(&target)>0);assert(f.m_featureOsdFont.text.back()==L"24fps -> 48fps");}
    for(int scenario=0;scenario<7;++scenario){CDX11VideoProcessor f;f.m_iVPSuperRes=SUPERRES_Disable;f.m_mfgMultiplier=mfg::AutoMultiplier;
        if(scenario==0)f.m_mfgMultiplier=0;
        if(scenario==1)f.bypass=true;
        if(scenario==2)f.m_mfg.fail=true;
        if(scenario==3)f.m_mfgPresentFailed=true;
        if(scenario==4)f.m_rtAvgTimePerFrame=UNITS/60;
        if(scenario==5)f.m_pPSHalfOUtoInterlace=true;
        if(scenario==6)f.m_mfg.cap=1;
        tick=50000;assert(!f.DrawFeatureStatus(&target));tick=51000;assert(!f.DrawFeatureStatus(&target));}
    // Independent scaling and single rectangle/text pass; no Statistics atlas change.
    for (auto dimensions : {std::pair{640,360},std::pair{1280,720},std::pair{1920,1080},std::pair{3840,2160},std::pair{7680,4320}}) {
        CDX11VideoProcessor f;f.m_mfgMultiplier=mfg::AutoMultiplier;f.m_StatsFontH=14;
        target.w=dimensions.first;target.h=dimensions.second;
        tick=60000;assert(!f.DrawFeatureStatus(&target));tick=60600;assert(f.DrawFeatureStatus(&target)>0);
        const int expected=dimensions.second<=720?18:dimensions.second==1080?28:dimensions.second==2160?54:72;
        assert(f.m_featureOsdFont.height==expected);assert(f.m_featureOsdFont.coords.size()==1);
        assert(f.m_featureOsdBackground.count==1 && f.m_featureOsdBackground.color==0xcc000000);
        assert(f.m_Font3D.height==14 && f.m_Font3D.text.empty());
        const int atlases=f.m_featureOsdFont.atlases;
        for(int i=1;i<=20;++i){tick=60600+17*i;assert(f.DrawFeatureStatus(&target)>0);}
        assert(f.m_featureOsdFont.inits==1 && f.m_featureOsdFont.atlases==atlases && f.m_featureOsdBackground.count==21);
    }
    target.w=1280;target.h=720;
    // Failure never paints unbacked text or acknowledges the notice.
    {CDX11VideoProcessor f;f.m_featureOsdBackground.fail=true;tick=70000;assert(!f.DrawFeatureStatus(&target));
     tick=70600;assert(!f.DrawFeatureStatus(&target));assert(f.m_featureOsdFont.text.empty());
     f.m_featureOsdBackground.fail=false;tick=70617;assert(f.DrawFeatureStatus(&target)>0);}
    {CDX11VideoProcessor f;f.m_featureOsdFont.initFail=true;tick=71000;assert(!f.DrawFeatureStatus(&target));
     tick=71600;assert(!f.DrawFeatureStatus(&target));tick=71617;assert(!f.DrawFeatureStatus(&target));
     assert(f.m_featureOsdFont.inits==1 && f.m_featureOsdFont.invalidates==1 && f.m_featureOsdBackground.count==0);}
    std::cout<<"PASS FeatureOsdDraw: actual methods, size, colors, timer, pause/flush, stats priority, independent toggle and new run\n";
}
}
void TestFeatureOsdDraw(){Exercise();}
#ifdef FEATURE_OSD_DRAW_STANDALONE
int main(){TestFeatureOsdDraw();}
#endif
