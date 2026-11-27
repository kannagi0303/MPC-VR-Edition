// SPDX-License-Identifier: GPL-3.0-or-later
// Tests the actual extracted producer-stage methods, not a second implementation.
#undef NDEBUG
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include "Edition/AdaptiveToneShot.h"

namespace {
using REFERENCE_TIME=int64_t; using HRESULT=int;
constexpr int S_OK=0,S_FALSE=1,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709=0;
constexpr int64_t INVALID_TIME=INT64_MIN;
#define FAILED(x) ((x)<0)
struct ID3D11Texture2D {int id;}; struct ID3D11ShaderResourceView {int id;};
struct Size {int cx,cy;bool operator==(const Size&)const=default;};
struct CRect {int l,t,r,b; CRect(int a=0,int c=0,int d=0,int e=0):l(a),t(c),r(d),b(e){} int Width()const{return r-l;}int Height()const{return b-t;} ::Size Size()const{return {Width(),Height()};} bool operator==(const CRect&)const=default;bool IntersectRect(CRect a,CRect z){l=std::max(a.l,z.l);t=std::max(a.t,z.t);r=std::min(a.r,z.r);b=std::min(a.b,z.b);return Width()>0&&Height()>0;}};
struct CPoint {int x,y;};
constexpr int Tex2D_DefaultShaderRTarget=1;
bool Supported(int f){return f==87;}
struct Ptr {ID3D11Texture2D* p=nullptr; operator ID3D11Texture2D*()const{return p;}};
struct Tex2D_t {Ptr pTexture; bool pShaderResource=true;struct Desc{int Format=87,Width=1920,Height=1080;}desc; ID3D11Texture2D out{2}; bool fail=false;int allocations=0;int CheckCreate(int,int,int,int,int){++allocations;if(fail)return -1;pTexture.p=&out;return S_OK;}};
namespace svdlut {struct ModelWorker {enum class State{Ready,Loading};State state=State::Ready;State GetState()const{return state;}};}
struct CDX11VideoProcessor {
 struct Cancel{bool cancelled=false;Cancel* Capture(){return this;}bool Cancelled(){return cancelled;}};
 struct Filter{bool m_bFlushing=false;std::atomic_bool m_videoEnhancement{false},m_memoryColor{false},m_skinTone{false};std::atomic_int m_adaptiveTone{100};Cancel m_playbackCancel;}filter;Filter* m_pFilter=&filter;
 std::optional<int> m_completedTarget{1};
 bool m_completingSample=true,m_fastShutdown=false,tone=true,hdr=false,m_bVPUseRTXVideoHDR=false,m_pPSHalfOUtoInterlace=false,m_bInterlaced=false;
 int m_currentSwapChainColorSpace=0;
 bool OptionalColorSupported()const{return tone;}bool SourceIsHDR()const{return hdr;}bool PerceptualEnabled()const{return filter.m_videoEnhancement||filter.m_memoryColor||filter.m_skinTone;}
 struct A {edition::adaptive_tone::Shot shot; bool sourceComposition=false,allowDraw=false,applied=false;Tex2D_t sourceOutput;}m_adaptiveTone;
 struct Early{bool ready=false,approved=false,consumed=false,srSource=false,domain=true;REFERENCE_TIME pts=100;Tex2D_t rgb;}m_earlyOriginal,m_futureOriginal;
 int finishes=0,discards=0;bool route=true;
 void FinishEarlyOriginal(){++finishes;m_earlyOriginal={};}void DiscardEarlyOriginal(Early&){++discards;}
 bool EarlyOriginalDomainMatches(const Early& e){return e.domain;}bool EarlyOriginalRoute(CRect,CRect){return route;}
 CRect m_srcRect{0,0,1920,1080},m_renderRect=m_srcRect,m_videoRect=m_srcRect;CRect ProcessingRect(){return m_renderRect;}CRect ProcessingSourceRect(bool){return m_srcRect;}
 bool project=false,depthEligible=true,srDepth=false,mfgDepth=false;bool CanProjectJvr(){return project;}bool MfgDepthSourceEligible(){return depthEligible;}bool UsesDepthSr(){return srDepth;}bool UsesDepthMfg(){return mfgDepth;}
 int m_pDevice=0;struct Context{int unbind=0,srv=0;void OMSetRenderTargets(int,void*,void*){++unbind;}void PSSetShaderResources(int,int,ID3D11ShaderResourceView** p){assert(!*p);++srv;}}context;Context* m_pDeviceContext=&context;
 struct Depth{int updates=0,polls=0;bool match=true;void Update(int,Context*,bool,CRect,Size,REFERENCE_TIME){++updates;}void PollCompleted(){++polls;}bool MatchesColorSource(int,int){return match;}}m_depth;
 struct B{bool active=true,failed=false,attempted=false;REFERENCE_TIME activationPts=INVALID_TIME;svdlut::ModelWorker model;}m_svdLutRawTest;
 int polls=0,observes=0,confirms=0,draws=0,result=S_OK,lutResult=S_OK,perceptualResult=S_OK,luts=0,perceptuals=0,starts=0,analyses=0,resourceResult=S_OK;bool mayStart=true,depthForGrade=false;const Tex2D_t* analyzed=nullptr;
 void ConfirmEarlyAdaptiveTone(const Tex2D_t&,const CRect&,REFERENCE_TIME){++confirms;}
 void PollAdaptiveTone(REFERENCE_TIME){++polls;assert(m_adaptiveTone.sourceComposition);}
 void ObserveAdaptiveTone(const Tex2D_t& s,const CRect&,REFERENCE_TIME,bool buffered){assert(!buffered);++observes;analyzed=&s;}
 bool VideoEnhancementMayStart(bool){return mayStart;}void StartSvdLutRawTest(){++starts;m_svdLutRawTest.attempted=true;}int PrepareSvdLutRawResources(){return resourceResult;}
 bool BeginSvdLutAnalysis(const Tex2D_t&,CRect,REFERENCE_TIME,int,bool){++analyses;return false;}
 int ApplySvdLutRawFrame(const Tex2D_t&,ID3D11Texture2D*,CRect,CRect,CPoint,REFERENCE_TIME,int,bool,bool depth,bool){assert(!m_futureOriginal.ready || m_earlyOriginal.consumed);++luts;depthForGrade=depth;return lutResult;}
 int ApplyPerceptual(const Tex2D_t&,ID3D11Texture2D*,CRect,CRect,CPoint,REFERENCE_TIME,int,void*,std::array<float,3>,bool,float,bool depth){++perceptuals;depthForGrade=depth;return perceptualResult;}
 int DrawAdaptiveTone(const Tex2D_t& src,ID3D11Texture2D* target,const CRect&,const CRect&,REFERENCE_TIME pts,const CRect*){assert(src.pTexture.p!=target);assert(m_adaptiveTone.allowDraw&&m_adaptiveTone.sourceComposition);assert(AdaptiveToneWeight(pts)>0);++draws;return result;}
 float AdaptiveToneWeight(REFERENCE_TIME)const; bool SourceToneEligible()const;
 ID3D11Texture2D* PrepareSourceTone(const Tex2D_t&,const CRect&,REFERENCE_TIME,ID3D11Texture2D* directTarget=nullptr);
};
#include "SourceToneMethods.generated.inc"
void Exercise(){using P=CDX11VideoProcessor;unsigned cases=0;
 for(unsigned mask=0;mask<2048;++mask){P p;p.m_completingSample=mask&1;if(mask&2)p.m_completedTarget.reset();p.tone=mask&4;p.filter.m_videoEnhancement=bool(mask&8);p.hdr=mask&16;p.m_bVPUseRTXVideoHDR=mask&32;p.m_pPSHalfOUtoInterlace=mask&64;p.m_currentSwapChainColorSpace=(mask&128)?1:0;p.m_fastShutdown=mask&256;p.filter.m_bFlushing=mask&512;p.filter.m_playbackCancel.cancelled=mask&1024;
  const bool enabled=((mask&1)&&!(mask&2))||(mask&4)||(mask&8);assert(p.SourceToneEligible()==(enabled&&!(mask&2032)));++cases;}
 ID3D11Texture2D raw{1},target{3},other{4};Tex2D_t input;input.pTexture.p=&raw;CRect area(0,0,1920,1080);
 P p;assert(p.PrepareSourceTone(input,area,100)==&raw);assert(p.observes==1&&!p.draws&&!p.m_adaptiveTone.sourceOutput.allocations);assert(!p.m_adaptiveTone.sourceComposition&&!p.m_adaptiveTone.allowDraw);
 auto& a=p.m_adaptiveTone;a.shot.active=true;a.shot.locked=a.shot.activation=200;
 assert(p.PrepareSourceTone(input,area,199)==&raw);assert(p.AdaptiveToneWeight(199)==0&&p.AdaptiveToneWeight(200)==1);
 assert(p.PrepareSourceTone(input,area,200)==&a.sourceOutput.out);assert(p.draws==1&&p.analyzed==&input&&!a.sourceComposition&&!a.allowDraw);
 auto allocations=a.sourceOutput.allocations;assert(p.PrepareSourceTone(input,area,201,&target)==&target);assert(a.sourceOutput.allocations==allocations);assert(p.PrepareSourceTone(input,area,201,&raw)==&raw);
 p.result=-1;assert(p.PrepareSourceTone(input,area,201,&target)==&raw);assert(!a.sourceComposition&&!a.allowDraw);a.sourceOutput.fail=true;assert(p.PrepareSourceTone(input,area,202)==&raw);assert(!a.sourceComposition&&!a.allowDraw);
 p.filter.m_adaptiveTone=35;assert(p.AdaptiveToneWeight(201)==.35f);
 for(unsigned mask=0;mask<256;++mask){P q;q.filter.m_videoEnhancement=true;q.m_earlyOriginal.ready=q.m_earlyOriginal.approved=true;q.m_earlyOriginal.consumed=!(mask&1);q.m_earlyOriginal.rgb.pTexture.p=(mask&2)?&other:&raw;q.m_earlyOriginal.pts=(mask&4)?99:100;q.m_futureOriginal.ready=true;q.m_futureOriginal.srSource=mask&8;q.m_earlyOriginal.domain=!(mask&16);q.m_futureOriginal.domain=!(mask&32);q.route=!(mask&64);auto pic=(mask&128)?CRect(0,0,1919,1080):area;
  assert(q.PrepareSourceTone(input,pic,100,&target)==&target);assert(q.discards==int(mask!=0));assert(q.m_futureOriginal.ready==(mask==0));assert(q.finishes==int(bool(mask&6)));assert(q.luts==1&&q.observes==1);++cases;}
 for(unsigned mask=0;mask<128;++mask){P q;q.filter.m_videoEnhancement=true;q.m_adaptiveTone.shot.active=true;q.m_adaptiveTone.shot.locked=q.m_adaptiveTone.shot.activation=0;q.srDepth=mask&1;q.mfgDepth=mask&2;q.lutResult=(mask&4)?S_FALSE:S_OK;q.perceptualResult=(mask&8)?-1:S_OK;q.result=(mask&16)?-1:S_OK;q.resourceResult=(mask&32)?-1:S_OK;q.m_svdLutRawTest.model.state=(mask&64)?svdlut::ModelWorker::State::Loading:svdlut::ModelWorker::State::Ready;
  bool lut=!(mask&96),lutOK=lut&&!(mask&4),perceptual=!lutOK,perceptualOK=perceptual&&!(mask&8),at=perceptual&&!perceptualOK,ok=lutOK||perceptualOK||(at&&!(mask&16));assert(q.PrepareSourceTone(input,area,100,&target)==(ok?&target:&raw));assert(q.luts==int(lut)&&q.analyses==int(lut)&&q.perceptuals==int(perceptual)&&q.draws==int(at));assert(q.observes==1&&q.confirms==1&&q.polls==1);assert(q.m_depth.updates==int(bool(mask&3))&&q.m_depth.polls==int(bool(mask&3)));assert(!q.m_adaptiveTone.sourceComposition&&!q.m_adaptiveTone.allowDraw);++cases;}
 P cancelled;cancelled.filter.m_playbackCancel.cancelled=true;assert(cancelled.PrepareSourceTone(input,area,0)==&raw&&!cancelled.observes);P h;h.hdr=true;assert(h.PrepareSourceTone(input,area,0)==&raw&&!h.observes);
 std::cout<<"PASS "<<cases<<" source-tone eligibility, early/future invalidation and color/depth fallback cases plus targeted activation checks\n";
}
}
void TestSourceToneRouting(){Exercise();}
