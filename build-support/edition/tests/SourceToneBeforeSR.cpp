// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {
using REFERENCE_TIME=int64_t;
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT=10;
#define FAILED(x) ((x)<0)
struct CRect{int l,t,r,b;bool operator==(const CRect&) const=default;int Size()const{return r*b;}};
struct ID3D11Texture2D{int n;};struct ID3D11ShaderResourceView{int n;};
template<class T>struct Ptr{T* p=nullptr;operator T*()const{return p;}};
struct Tex2D_t{struct{int Width=1920,Height=1080,Format=10;}desc;Ptr<ID3D11Texture2D>pTexture;Ptr<ID3D11ShaderResourceView>pShaderResource;};
struct CDX11VideoProcessor{
 bool m_dlssSRAfterMfg=false,eligible=true,overlayEnabled=false,srDepth=false,mfgDepth=false,drawOK=true,copyOK=true,evalOK=true;
 struct Cancel{bool cancelled=false;Cancel* Capture(){return this;}void* Event(){return nullptr;}bool Cancelled()const{return cancelled;}};
 struct Filter{Cancel m_playbackCancel;}filter;Filter* m_pFilter=&filter;REFERENCE_TIME m_sampleDuration=400000;
 int m_mfgMultiplier=2,m_pPS_Simple=0,m_pDevice=0,draws=0,copies=0;const wchar_t* m_dlssSRInputStatus=nullptr;
 struct{bool sourceToneBeforeUpscale=false;std::optional<bool>srSourceToneRoute;}m_adaptiveTone;
 struct Context{void OMSetRenderTargets(int,void*,void*){}void PSSetShaderResources(int,int,ID3D11ShaderResourceView**){}}ctx;Context*m_pDeviceContext=&ctx;
 struct Depth{int calls=0,awaits=0;ID3D11ShaderResourceView* raw=nullptr;void Update(int,Context*,ID3D11ShaderResourceView* p,CRect,int,REFERENCE_TIME){++calls;raw=p;}void AwaitOriginal(REFERENCE_TIME,REFERENCE_TIME,void*){++awaits;}}m_depth;
 struct SR{CDX11VideoProcessor* owner;int resets=0,calls=0;ID3D11ShaderResourceView* clean=nullptr;Tex2D_t result;void ResetHistory(){++resets;}Tex2D_t* Evaluate(REFERENCE_TIME,float,Depth*,bool,ID3D11ShaderResourceView* p){++calls;clean=p;return owner->evalOK?&result:nullptr;}}m_dlssSR{this};
 bool SourceToneEligible()const{return eligible;}bool DepthOverlayEnabled()const{return overlayEnabled;}bool UsesDepthSr()const{return srDepth;}bool UsesDepthMfg()const{return mfgDepth;}
 ID3D11Texture2D* PrepareSourceTone(const Tex2D_t& src,CRect,REFERENCE_TIME,ID3D11Texture2D* target){++draws;assert(src.pTexture.p!=target);return drawOK?target:src.pTexture.p;}
 int TextureCopyRect(const Tex2D_t&,ID3D11Texture2D*,CRect,CRect,int,void*,int,bool){++copies;return copyOK?0:-1;}
 bool BlendSourceDepthOverlay(ID3D11Texture2D*,ID3D11ShaderResourceView*,CRect,REFERENCE_TIME){return overlayEnabled;}
 Tex2D_t* EvaluateDlssSR(const Tex2D_t&,Tex2D_t&,const CRect&,REFERENCE_TIME,float,bool*,bool);
};

#include "DepthSourceToneSr.generated.inc"

void Exercise(){ID3D11Texture2D raw{1},dst{2};ID3D11ShaderResourceView rawView{1};Tex2D_t in,out;in.pTexture.p=&raw;in.pShaderResource.p=&rawView;out.pTexture.p=&dst;
 CRect rect{0,0,1920,1080};unsigned cases=0;
 for(unsigned mask=0;mask<4096;++mask){CDX11VideoProcessor p;
  p.m_dlssSRAfterMfg=mask&1;p.eligible=!(mask&2);p.overlayEnabled=mask&4;p.srDepth=mask&8;p.mfgDepth=mask&16;
  p.drawOK=!(mask&32);p.copyOK=!(mask&64);p.evalOK=!(mask&128);bool projected=mask&256;
  out.desc.Format=(mask&512)?87:10;auto pts=(mask&1024)?-1:100;p.filter.m_playbackCancel.cancelled=mask&2048;
  const bool tone=!p.m_dlssSRAfterMfg&&p.eligible&&!p.overlayEnabled&&out.desc.Format==10&&pts>=0;
  const bool depth=!p.m_dlssSRAfterMfg&&pts>=0&&(p.srDepth||p.mfgDepth);
  const bool fused=tone&&p.drawOK,copyNeeded=!fused,converted=fused||p.copyOK;
  const bool evaluated=converted&&!(depth&&p.filter.m_playbackCancel.cancelled),success=evaluated&&p.evalOK;
  bool overlay=false;auto* ret=p.EvaluateDlssSR(in,out,rect,pts,40,&overlay,projected);
  assert(bool(ret)==success);assert(p.draws==int(tone));assert(p.copies==int(copyNeeded));
  assert(p.m_adaptiveTone.sourceToneBeforeUpscale==(success&&tone));
  assert(p.m_depth.calls==int(depth&&!projected));assert(p.m_depth.awaits==int(depth&&converted));
  if(p.m_depth.calls)assert(p.m_depth.raw==&rawView);
  assert(p.m_dlssSR.calls==int(evaluated));
  if(evaluated)assert(p.m_dlssSR.clean==((projected||tone||depth)?&rawView:nullptr));
  ++cases;
 }
 out.desc.Format=10;CDX11VideoProcessor p;bool overlay=false;
 assert(p.EvaluateDlssSR(in,out,rect,100,40,&overlay,false));assert(p.m_dlssSR.resets==0);
 p.eligible=false;assert(p.EvaluateDlssSR(in,out,rect,200,40,&overlay,false));assert(p.m_dlssSR.resets==1&&!p.m_adaptiveTone.sourceToneBeforeUpscale);
 p.eligible=true;p.drawOK=false;p.copyOK=false;assert(!p.EvaluateDlssSR(in,out,rect,300,40,&overlay,false));assert(!p.m_adaptiveTone.sourceToneBeforeUpscale);
 std::cout<<"PASS "<<cases<<" actual EvaluateDlssSR routing combinations + route transitions; fused native draw replaces copy, raw depth, failure fallback, no generated grading\n";
}
}

void TestSourceToneBeforeSR() { Exercise(); }
