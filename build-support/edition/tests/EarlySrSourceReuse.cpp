// SPDX-License-Identifier: GPL-3.0-or-later
// Actual renderer methods, extracted by run-page-tests.ps1; no GPU emulation claim.
#undef NDEBUG
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {
using UINT=unsigned; using REFERENCE_TIME=int64_t;
constexpr auto INVALID_TIME=REFERENCE_TIME(-1);
constexpr int SUPERRES_Disable=0,SUPERRES_DLSS_SR=1,DXGI_FORMAT_NV12=2,DXGI_FORMAT_B8G8R8A8_UNORM=3,
 D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE=0,Tex2D_DefaultShaderRTarget=1;
#define FAILED(x) ((x)<0)
struct CRect { int l=0,t=0,r=0,b=0; bool operator==(const CRect&)const=default;
 int Width()const{return r-l;} int Height()const{return b-t;} bool IsRectEmpty()const{return Width()<=0||Height()<=0;}
 std::pair<int,int> Size()const{return {Width(),Height()};} };
struct Texture{};struct View{};struct Sample{};
template<class T> struct Ptr{T* p=nullptr;explicit operator bool()const{return p!=nullptr;}operator T*()const{return p;} void Release(){p=nullptr;}};
struct Tex2D_t { Ptr<Texture> pTexture; Ptr<View> pShaderResource; struct{UINT Width=1280,Height=720; int Format=DXGI_FORMAT_B8G8R8A8_UNORM;}desc;
 void Release(){pTexture.Release();pShaderResource.Release();} int CheckCreate(int,int,int,int,int){return pTexture?0:-1;} };
struct EarlyOriginalImage { Tex2D_t rgb; CRect rect;std::array<UINT,5> sourceKey{}; const Sample* sample=nullptr;const Texture* decoderTexture=nullptr;
 UINT decoderSlice=0;REFERENCE_TIME pts=INVALID_TIME;uint64_t epoch=0;bool srSource=false,ready=false,approved=false,consumed=false,captured=false; };
struct VP {bool ready=true;struct Held{Ptr<Sample>pSample;Ptr<Texture>pTexture;UINT ArraySlice=0;}held;bool IsReady()const{return ready;} Held HoldFrame(){return held;}void ReleaseVideoDevice(){ready=false;}};
struct EarlyOriginalState:EarlyOriginalImage {VP vp;Ptr<int>completion;std::array<UINT,5>key{};bool queryIssued=false,broken=false;};
struct CDX11VideoProcessor {
 enum class UpscaleSlot{BeforeMfg,AfterMfg}; bool before=true,sr=true,project=false,eligible=true,overlay=false,srcAllowed=true,inputOK=true,evalOK=true,vpOK=true;
 int m_pDevice=1,m_pDeviceContext=1,m_srcDXGIFormat=DXGI_FORMAT_NV12,m_D3D11OutputFmt=DXGI_FORMAT_B8G8R8A8_UNORM,
  m_srcWidth=1280,m_srcHeight=720,m_SampleFormat=0,m_iRotation=0,m_iVPSuperRes=SUPERRES_DLSS_SR,m_iVPDeinterlacing=0,m_rtAvgTimePerFrame=333667;
 static constexpr int DEINT_Disable=0;
 bool m_bInterlaced=false,m_bDoubleFrames=false,m_bFlip=false,m_bVPUseSuperRes=false,m_bVPRTXVideoHDR=false,m_bVPScaling=true,m_completingSample=true;
 struct{int CDepth=8;}m_srcParams;struct{bool bValid=false;}m_Dovi;struct{UINT value=123;}m_srcExFmt;
 struct Fixed{int ll=0;};struct{Fixed Brightness,Hue,Contrast{65536},Saturation{65536};}m_DXVA2ProcAmpValues;
 CRect m_srcRect{0,0,1280,720};VP m_D3D11VP;
 struct Filter{struct{bool value=true;bool load()const{return value;}}m_videoEnhancement;uint64_t m_completedEpoch=9;}filter;Filter*m_pFilter=&filter;
 struct Target{bool videoOnly=true;}target;Target*m_completedTarget=&target;
 EarlyOriginalState m_earlyOriginal; EarlyOriginalImage m_futureOriginal;
 struct{Tex2D_t sourceRaw;}m_adaptiveTone; Tex2D_t m_dlssSRSource,input,output;
 const wchar_t* m_dlssSRInputStatus=nullptr;int discard=0,discardsFuture=0,finish=0,vpCalls=0,evalCalls=0,prepare=0;
 const Tex2D_t* evaluated=nullptr;bool evalDepthPrepared=false;float frameTime=0;
 bool CanProjectJvr()const{return project;}bool SourceToneEligible()const{return eligible;}bool DepthOverlayEnabled()const{return overlay;}
 bool CanDlssSRUpscale(const CRect&,const CRect&,bool){return sr;}bool UpscaleAt(UpscaleSlot s)const{return before==(s==UpscaleSlot::BeforeMfg);}
 bool DlssSRSourceAllowed(const CRect*)const{return srcAllowed;}
 void DiscardEarlyAdaptiveTone(REFERENCE_TIME,uint64_t){}
 // The real invalidation algorithm is separately covered by existing LUT tests.
 void DiscardEarlyOriginal(EarlyOriginalImage& e){if(e.ready&&!e.consumed){++discard;if(&e==&m_futureOriginal)++discardsFuture;}}
 void FinishEarlyOriginal();
 Tex2D_t* PrepareDlssSR(const CRect&,const CRect&,bool after){assert(!after);++prepare;return inputOK?&input:nullptr;}
 int D3D11VPPass(Texture*,const CRect&,const CRect&,bool){++vpCalls;return vpOK?0:-1;}
 Tex2D_t* EvaluateDlssSR(const Tex2D_t& source,Tex2D_t&,const CRect&,REFERENCE_TIME,float ft,bool*,bool depthPrepared=false){++evalCalls;evaluated=&source;evalDepthPrepared=depthPrepared;frameTime=ft;return evalOK?&output:nullptr;}
 bool EarlyOriginalRoute(const CRect&,const CRect&);
 bool EarlyOriginalDomainMatches(const EarlyOriginalImage&)const;
 void ClearEarlyOriginal(bool release=false);
 const Tex2D_t* ReuseEarlyOriginal(const CRect&,const CRect&,REFERENCE_TIME,bool,bool=false);
 Tex2D_t* TryDlssSR(const CRect&,const CRect&,bool,REFERENCE_TIME,bool*,const Tex2D_t*);
};
#include "EarlySrSourceReuse.generated.inc"
Texture tex[5]; View views[5]; Sample samples[2];
Tex2D_t Surface(unsigned i){Tex2D_t x;x.pTexture.p=&tex[i];x.pShaderResource.p=&views[i];return x;}
void SetEarly(CDX11VideoProcessor& p,bool sr,bool future=false){auto& e=p.m_earlyOriginal;e.rgb=Surface(0);e.rect=p.m_srcRect;e.sourceKey={UINT(p.m_srcDXGIFormat),UINT(p.m_srcWidth),UINT(p.m_srcHeight),p.m_srcExFmt.value,UINT(p.m_D3D11OutputFmt)};
 e.sample=&samples[0];e.decoderTexture=&tex[3];e.decoderSlice=2;e.pts=100;e.epoch=p.filter.m_completedEpoch;e.ready=e.approved=e.captured=true;(void)future;e.srSource=sr;
 p.m_D3D11VP.held={{&samples[0]},{&tex[3]},2};}
void Exercise(){
 const CRect native{0,0,1280,720},large{0,0,3840,2160};unsigned routes=0,reuses=0,clears=0,attempts=0;
 for(unsigned mask=0;mask<(1u<<16);++mask){CDX11VideoProcessor p;p.sr=mask&1;p.before=mask&2;p.m_bVPScaling=mask&4;
  const auto dest=(mask&8)?large:native;p.m_bInterlaced=mask&16;p.m_bDoubleFrames=mask&32;p.m_iRotation=(mask&64)?90:0;p.m_bFlip=mask&128;
  p.project=mask&256;p.m_Dovi.bValid=mask&512;p.m_bVPUseSuperRes=mask&1024;p.m_bVPRTXVideoHDR=mask&2048;p.eligible=!(mask&4096);p.overlay=mask&8192;p.filter.m_videoEnhancement.value=!(mask&16384);p.m_DXVA2ProcAmpValues.Hue.ll=(mask&32768)?1:0;
  bool expected=(mask&0xfff0)==0 && (p.sr?p.before:dest==native);
  assert(p.EarlyOriginalRoute(native,dest)==expected);++routes;
 }
 for(unsigned mask=0;mask<(1u<<13);++mask){CDX11VideoProcessor p;const bool sr=mask&1,consumer=mask&2;SetEarly(p,sr,mask&4);p.sr=mask&8;p.before=!(mask&16);p.m_bVPScaling=false;
  auto dest=p.sr?large:native;const bool second=mask&32;auto pts=(mask&64)?101:100;p.m_earlyOriginal.approved=!(mask&128);p.m_completingSample=!(mask&256);p.target.videoOnly=!(mask&512);
  if(mask&1024)++p.filter.m_completedEpoch;if(mask&2048)p.m_D3D11VP.held.ArraySlice=3;if(mask&4096)p.m_earlyOriginal.rgb.pShaderResource.Release();
  bool expected=(sr==consumer&&sr==p.sr&&(!p.sr||p.before)&&(mask&0x1fe0)==0);
  auto* result=p.ReuseEarlyOriginal(native,dest,pts,second,consumer);assert(bool(result)==expected);
  assert(p.m_earlyOriginal.consumed==expected);
  if(result){assert(result->pTexture.p==&tex[0]);assert(p.ReuseEarlyOriginal(native,dest,pts,second,consumer)==result);assert(p.m_earlyOriginal.consumed);}
  ++reuses;
 }
 for(unsigned mask=0;mask<16;++mask){CDX11VideoProcessor p;bool sr=mask&1,release=mask&2,occupied=mask&4;SetEarly(p,sr);auto& owner=sr?p.m_dlssSRSource:p.m_adaptiveTone.sourceRaw;auto& other=sr?p.m_adaptiveTone.sourceRaw:p.m_dlssSRSource;
  if(occupied)owner=Surface(1);other=Surface(2);p.sr=mask&8;p.ClearEarlyOriginal(release);
  assert(!p.m_earlyOriginal.rgb.pTexture&&!p.m_earlyOriginal.srSource&&!p.m_earlyOriginal.ready);assert(other.pTexture.p==&tex[2]);
  assert(owner.pTexture.p==(occupied?&tex[1]:release?nullptr:&tex[0]));++clears;
 }
 for(unsigned mask=0;mask<256;++mask){CDX11VideoProcessor p;SetEarly(p,true);p.m_dlssSRSource=Surface(1);p.m_futureOriginal.ready=true;p.m_futureOriginal.rgb=Surface(2);p.m_futureOriginal.srSource=true;
  bool prepared=mask&1;p.inputOK=!(mask&2);p.evalOK=!(mask&4);p.vpOK=!(mask&8);p.m_earlyOriginal.approved=!(mask&16);p.m_bDoubleFrames=mask&32;const bool second=mask&64;const auto pts=(mask&128)?101:100;
  Tex2D_t external=Surface(4);auto* result=p.TryDlssSR(native,large,second,pts,nullptr,prepared?&external:nullptr);
  bool early=p.inputOK&&!prepared&&(mask&(16|32|64|128))==0;
  bool eval=p.inputOK&&(prepared||early||p.vpOK);assert(p.evalCalls==int(eval));assert(p.vpCalls==int(p.inputOK&&!prepared&&!early));assert(bool(result)==(eval&&p.evalOK));
  if(eval){assert(p.evalDepthPrepared==prepared);if(early&&p.evalOK)assert(p.evaluated==&p.m_earlyOriginal.rgb);if(early&&!p.evalOK)assert(!p.m_futureOriginal.ready&&!p.m_earlyOriginal.ready&&p.discard==2);if(prepared)assert(p.evaluated==&external);}
  ++attempts;
 }
 std::cout<<"PASS early SR: "<<routes<<" eligibility, "<<reuses<<" reuse, "<<clears<<" scratch returns, "<<attempts<<" SR calls\n";
}
}
void TestEarlySrSourceReuse(){Exercise();}
