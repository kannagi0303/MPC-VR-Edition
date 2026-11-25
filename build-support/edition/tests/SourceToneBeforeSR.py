# SPDX-License-Identifier: GPL-3.0-or-later
# Extract the actual FA-03B evaluation entry; only GPU collaborators are stubs.
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'Source/Edition/DX11SuperResolution.cpp').read_text()
start=s.index('Tex2D_t* CDX11VideoProcessor::EvaluateDlssSR(')
end=s.index('Tex2D_t* CDX11VideoProcessor::TryDlssSR(',start)
code=r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
using REFERENCE_TIME=int64_t;
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT=10;
#define FAILED(x) ((x)<0)
struct CRect{int l,t,r,b;bool operator==(const CRect&) const=default;int Size()const{return r*b;}};
struct ID3D11Texture2D{int n;};struct ID3D11ShaderResourceView{int n;};
template<class T>struct Ptr{T* p=nullptr;operator T*()const{return p;}};
struct Tex2D_t{struct{int Width=1920,Height=1080,Format=10;}desc;Ptr<ID3D11Texture2D>pTexture;Ptr<ID3D11ShaderResourceView>pShaderResource;};
struct CDX11VideoProcessor{
 bool m_dlssSRAfterMfg=false,eligible=true,overlayEnabled=false,srDepth=false,mfgDepth=false,drawOK=true,copyOK=true,evalOK=true;
 int m_mfgMultiplier=2,m_pPS_Simple=0,m_pDevice=0,draws=0,copies=0;const wchar_t* m_dlssSRInputStatus=nullptr;
 struct{bool sourceToneBeforeUpscale=false;std::optional<bool>srSourceToneRoute;}m_adaptiveTone;
 struct Context{void OMSetRenderTargets(int,void*,void*){}void PSSetShaderResources(int,int,ID3D11ShaderResourceView**){}}ctx;Context*m_pDeviceContext=&ctx;
 struct Depth{int calls=0;ID3D11ShaderResourceView* raw=nullptr;void Update(int,Context*,ID3D11ShaderResourceView* p,CRect,int,REFERENCE_TIME){++calls;raw=p;}}m_zipDepth;
 struct SR{CDX11VideoProcessor* owner;int resets=0,calls=0;ID3D11ShaderResourceView* clean=nullptr;Tex2D_t result;void ResetHistory(){++resets;}Tex2D_t* Evaluate(REFERENCE_TIME,float,Depth*,bool,ID3D11ShaderResourceView* p){++calls;clean=p;return owner->evalOK?&result:nullptr;}}m_dlssSR{this};
 bool SourceToneEligible()const{return eligible;}bool DepthOverlayEnabled()const{return overlayEnabled;}bool UsesZipDepthSr()const{return srDepth;}bool UsesZipDepthMfg()const{return mfgDepth;}
 ID3D11Texture2D* PrepareSourceTone(const Tex2D_t& src,CRect,REFERENCE_TIME,ID3D11Texture2D* target){++draws;assert(src.pTexture.p!=target);return drawOK?target:src.pTexture.p;}
 int TextureCopyRect(const Tex2D_t&,ID3D11Texture2D*,CRect,CRect,int,void*,int,bool){++copies;return copyOK?0:-1;}
 bool BlendSourceDepthOverlay(ID3D11Texture2D*,ID3D11ShaderResourceView*,CRect,REFERENCE_TIME){return overlayEnabled;}
 Tex2D_t* EvaluateDlssSR(const Tex2D_t&,Tex2D_t&,const CRect&,REFERENCE_TIME,float,bool*,bool);
};
'''+s[start:end]+r'''
int main(){ID3D11Texture2D raw{1},dst{2};ID3D11ShaderResourceView rawView{1};Tex2D_t in,out;in.pTexture.p=&raw;in.pShaderResource.p=&rawView;out.pTexture.p=&dst;
 CRect rect{0,0,1920,1080};unsigned cases=0;
 for(unsigned mask=0;mask<2048;++mask){CDX11VideoProcessor p;
  p.m_dlssSRAfterMfg=mask&1;p.eligible=!(mask&2);p.overlayEnabled=mask&4;p.srDepth=mask&8;p.mfgDepth=mask&16;
  p.drawOK=!(mask&32);p.copyOK=!(mask&64);p.evalOK=!(mask&128);bool projected=mask&256;
  out.desc.Format=(mask&512)?87:10;auto pts=(mask&1024)?-1:100;
  const bool tone=!p.m_dlssSRAfterMfg&&p.eligible&&!p.overlayEnabled&&out.desc.Format==10&&pts>=0;
  const bool fused=tone&&p.drawOK,copyNeeded=!fused,converted=fused||p.copyOK,success=converted&&p.evalOK;
  bool overlay=false;auto* ret=p.EvaluateDlssSR(in,out,rect,pts,40,&overlay,projected);
  assert(bool(ret)==success);assert(p.draws==int(tone));assert(p.copies==int(copyNeeded));
  assert(p.m_adaptiveTone.sourceToneBeforeUpscale==(success&&tone));
  assert(p.m_zipDepth.calls==int(converted&&tone&&!projected&&(p.srDepth||p.mfgDepth)));
  if(p.m_zipDepth.calls)assert(p.m_zipDepth.raw==&rawView);
  if(converted)assert(p.m_dlssSR.clean==((projected||tone)?&rawView:nullptr));
  ++cases;
 }
 out.desc.Format=10;CDX11VideoProcessor p;bool overlay=false;
 assert(p.EvaluateDlssSR(in,out,rect,100,40,&overlay,false));assert(p.m_dlssSR.resets==0);
 p.eligible=false;assert(p.EvaluateDlssSR(in,out,rect,200,40,&overlay,false));assert(p.m_dlssSR.resets==1&&!p.m_adaptiveTone.sourceToneBeforeUpscale);
 p.eligible=true;p.drawOK=false;p.copyOK=false;assert(!p.EvaluateDlssSR(in,out,rect,300,40,&overlay,false));assert(!p.m_adaptiveTone.sourceToneBeforeUpscale);
 std::cout<<"PASS "<<cases<<" actual EvaluateDlssSR routing combinations + route transitions; fused native draw replaces copy, raw depth, failure fallback, no generated grading\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(code);e=Path(d)/'test'
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-O2',str(p),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
# Static integration: not a driver/GPU test.
pipeline=(root/'Source/Edition/DX11VideoPipeline.cpp').read_text()
assert pipeline.index('m_adaptiveTone.sourceToneBeforeUpscale=false;')<pipeline.index('const auto result = Process(')
assert pipeline.index('if (m_adaptiveTone.sourceToneBeforeUpscale)')<pipeline.index('QueueCompletedFrame(std::move(*m_completedTarget))')
mfg=(root/'Source/DX11VideoProcessorMfg.cpp').read_text()
assert 'if(!upstreamTone && SourceToneEligible())' in mfg
assert 'm_mfgLastToneBeforeUpscale != upstreamTone' in mfg
assert 'if(upstreamTone && depth) depth->captureSource=false;' in mfg
assert 'm_completedTarget->sourceColorFinal=sourceColorFinal;' in mfg
print('PASS static producer/queue/presenter handoff guards')
