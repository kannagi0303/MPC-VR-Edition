// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <memory>
#include "Edition/AdaptiveToneShot.h"

namespace {
using REFERENCE_TIME=int64_t;
constexpr int S_OK=0,S_FALSE=1;
#define FAILED(x) ((x)<0)
struct ID3D11Texture2D {int id;};
struct Rect {int l,t,r,b; Rect(int a=0,int c=0,int d=0,int e=0):l(a),t(c),r(d),b(e){} int Width()const{return r-l;}int Height()const{return b-t;}bool IntersectRect(Rect a,Rect z){l=std::max(a.l,z.l);t=std::max(a.t,z.t);r=std::min(a.r,z.r);b=std::min(a.b,z.b);return Width()>0&&Height()>0;}};
using CRect=Rect;
constexpr int Tex2D_DefaultShaderRTarget=1;
bool Supported(int f){return f==87;}
struct Ptr {ID3D11Texture2D* p=nullptr; operator ID3D11Texture2D*()const{return p;}};
struct Tex2D_t {Ptr pTexture; bool pShaderResource=true;struct Desc{int Format=87,Width=1920,Height=1080;}desc; ID3D11Texture2D out{2}; bool fail=false;int allocations=0;int CheckCreate(int,int,int,int,int){++allocations;if(fail)return -1;pTexture.p=&out;return S_OK;}};
struct CDX11VideoProcessor {
 struct Cancel{bool cancelled=false;Cancel* Capture(){return this;}bool Cancelled(){return cancelled;}};
 struct Filter{bool m_bFlushing=false;std::atomic_bool m_videoEnhancement{false},m_memoryColor{false},m_skinTone{false};std::atomic_int m_adaptiveTone{100};Cancel m_playbackCancel;}filter;Filter* m_pFilter=&filter;
 struct Frame{bool sourceColorFinal=true;};struct Batch{Frame target;};
 std::optional<Frame> m_completedTarget{Frame{}},m_completedLast;
 std::deque<Frame> m_completedFrames;std::deque<Batch> m_completedBatches;
 bool m_completingSample=true,m_fastShutdown=false,tone=true;
 bool OptionalColorSupported()const{return tone;}bool AdaptiveToneSupported()const{return tone;}bool PerceptualEnabled()const{return filter.m_videoEnhancement||filter.m_memoryColor||filter.m_skinTone;}
 struct A {edition::adaptive_tone::Shot shot; bool sourceComposition=false,allowDraw=false,applied=false,shaderFailed=false,shader=true;struct Job{std::atomic_bool ready{false};};std::shared_ptr<Job> job;Tex2D_t sourceOutput;}m_adaptiveTone;
 int m_pDevice=0;struct Context{int unbind=0;void OMSetRenderTargets(int,void*,void*){++unbind;}}context;Context* m_pDeviceContext=&context;
 int polls=0,observes=0,draws=0,result=S_OK; const Tex2D_t* analyzed=nullptr;
 void PollAdaptiveTone(REFERENCE_TIME){++polls;assert(m_adaptiveTone.sourceComposition);}
 void ObserveAdaptiveTone(const Tex2D_t& s,const CRect&,REFERENCE_TIME,bool buffered){assert(!buffered);++observes;analyzed=&s;}
 int DrawAdaptiveTone(const Tex2D_t& src,ID3D11Texture2D* target,const CRect&,const CRect&,REFERENCE_TIME pts,const CRect*){assert(src.pTexture.p!=target);assert(m_adaptiveTone.allowDraw&&m_adaptiveTone.sourceComposition);assert(AdaptiveToneWeight(pts)>0);++draws;return result;}
 float AdaptiveToneWeight(REFERENCE_TIME)const; bool SourceToneEligible()const;
 ID3D11Texture2D* PrepareSourceTone(const Tex2D_t&,const CRect&,REFERENCE_TIME,ID3D11Texture2D* directTarget=nullptr);
};

#include "SourceToneMethods.generated.inc"

void Exercise(){using P=CDX11VideoProcessor;
 unsigned cases=0;
 for(int mask=0;mask<512;++mask){P p;p.m_completingSample=!(mask&1);if(mask&2)p.m_completedTarget.reset();p.tone=!(mask&4);p.filter.m_videoEnhancement=mask&8;p.m_fastShutdown=mask&16;p.filter.m_bFlushing=mask&32;p.filter.m_playbackCancel.cancelled=mask&64;if(mask&128)p.m_completedLast=P::Frame{false};if(mask&256)p.m_completedFrames.push_back({false});assert(p.SourceToneEligible()==(mask==0));++cases;}
 P mixed;mixed.m_completedFrames.push_back({true});mixed.m_completedBatches.push_back({{true}});mixed.m_completedLast=P::Frame{true};assert(mixed.SourceToneEligible());mixed.m_completedBatches.push_back({{false}});assert(!mixed.SourceToneEligible());
 ID3D11Texture2D raw{1};Tex2D_t input;input.pTexture.p=&raw;CRect area(0,0,1920,1080);
 P p;assert(p.PrepareSourceTone(input,area,100)==&raw);assert(p.observes==1&&!p.draws&&p.m_adaptiveTone.sourceOutput.allocations==0);assert(!p.m_adaptiveTone.sourceComposition&&!p.m_adaptiveTone.allowDraw);
 auto& a=p.m_adaptiveTone;a.shot.active=true;a.shot.locked=a.shot.activation=200;
 assert(p.PrepareSourceTone(input,area,199)==&raw);assert(!p.draws);assert(p.AdaptiveToneWeight(199)==0 && p.AdaptiveToneWeight(200)==1 && p.AdaptiveToneWeight(201)==1); // legacy now activates immediately
 assert(p.PrepareSourceTone(input,area,200)==&a.sourceOutput.out);assert(p.draws==1&&p.analyzed==&input&&p.context.unbind==1);assert(!a.sourceComposition&&!a.allowDraw);
 ID3D11Texture2D srInput{3};const int allocations=a.sourceOutput.allocations;
 assert(p.PrepareSourceTone(input,area,201,&srInput)==&srInput);assert(a.sourceOutput.allocations==allocations);
 assert(p.PrepareSourceTone(input,area,201,&raw)==&raw);
 p.result=-1;assert(p.PrepareSourceTone(input,area,201,&srInput)==&raw);assert(a.sourceOutput.allocations==allocations);
 p.result=-1;assert(p.PrepareSourceTone(input,area,201)==&raw);assert(!a.sourceComposition&&!a.allowDraw);
 p.result=S_OK;a.sourceOutput.fail=true;assert(p.PrepareSourceTone(input,area,202)==&raw);assert(!a.sourceComposition&&!a.allowDraw);
 P w;w.tone=false;assert(!w.SourceToneEligible());assert(w.PrepareSourceTone(input,area,0)==&raw);
 P mem;mem.filter.m_memoryColor=true;assert(!mem.SourceToneEligible());
 P skin;skin.filter.m_skinTone=true;assert(!skin.SourceToneEligible());
 p.filter.m_adaptiveTone=35;assert(p.AdaptiveToneWeight(201)==.35f);
 P cancelled;cancelled.filter.m_playbackCancel.cancelled=true;assert(cancelled.PrepareSourceTone(input,area,0)==&raw&&!cancelled.observes&&!cancelled.draws);
 P hdr;input.desc.Format=10;assert(hdr.PrepareSourceTone(input,area,0)==&raw&&!hdr.observes);input.desc.Format=87;
 P unavailable;unavailable.m_adaptiveTone.shot.active=true;unavailable.m_adaptiveTone.shot.locked=unavailable.m_adaptiveTone.shot.activation=0;unavailable.m_adaptiveTone.shader=false;assert(unavailable.PrepareSourceTone(input,area,0)==&raw&&!unavailable.draws&&!unavailable.m_adaptiveTone.sourceOutput.allocations);
 std::cout<<"PASS "<<cases<<" source-route combinations; raw fallback, independent perceptual routes, immediate source/legacy activation, cancellation, shader/texture failure\n";
}
}

void TestSourceToneRouting() { Exercise(); }
