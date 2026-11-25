# SPDX-License-Identifier: GPL-3.0-or-later
# FA-01: exercise production draw routing with fake backends; no GPU claim.
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'Source/DX11VideoProcessorSvdLut.cpp').read_text()
raw=(root/'Source/Edition/SvdLutRawMode.inc').read_text()
def method(name):
    start=s.index(name); a=s.index('{',start); n=1; b=a+1
    while n:
        n+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[start:b]
code=r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <iostream>
using HRESULT=int; using REFERENCE_TIME=int64_t;
constexpr int S_OK=0,S_FALSE=1;constexpr auto INVALID_TIME=-1;
struct CompletedFrame{}; struct ID3D11Texture2D{}; struct CRect{};
struct CDX11VideoProcessor{
 struct Filter{std::atomic_bool m_videoEnhancement{true},m_memoryColor{false},m_skinTone{false};bool m_bFlushing=false;} filter;Filter* m_pFilter=&filter;
 struct B{bool active=false;REFERENCE_TIME activationPts=INVALID_TIME;}m_svdLutRawTest;
 bool m_fastShutdown=false,admit=false,grade=false,perceptualReady=true;int raw=0,skin=0;
 bool PerceptualEnabled()const;
 bool VideoEnhancementMayStart(bool){return admit;}
 HRESULT DrawCompletedSvdLutRaw(const CompletedFrame&,ID3D11Texture2D*,const CRect&,bool){++raw;return grade?S_OK:S_FALSE;}
 HRESULT DrawCompletedPerceptual(const CompletedFrame&,ID3D11Texture2D*,const CRect&,bool){++skin;return perceptualReady?S_OK:S_FALSE;}
 void DrawSvdLutRaw(ID3D11Texture2D*,const CRect&,REFERENCE_TIME,bool){++raw;m_svdLutRawTest.active=grade;}
 void DrawPerceptual(ID3D11Texture2D*,const CRect&,REFERENCE_TIME){++skin;}
 HRESULT DrawCompletedSvdLut(const CompletedFrame&,ID3D11Texture2D*,const CRect&,bool);
 void DrawSvdLut(ID3D11Texture2D*,const CRect&,REFERENCE_TIME,bool);
};
'''+method('bool CDX11VideoProcessor::PerceptualEnabled(')+'\n'+method('HRESULT CDX11VideoProcessor::DrawCompletedSvdLut(')+'\n'+method('void CDX11VideoProcessor::DrawSvdLut(')+r'''
int main(){unsigned cases=0;
 for(int mask=0;mask<512;++mask){for(bool legacy:{false,true}){
  CDX11VideoProcessor p; p.filter.m_videoEnhancement=mask&1;p.filter.m_memoryColor=mask&128;p.filter.m_skinTone=mask&256;p.filter.m_bFlushing=mask&2;p.m_fastShutdown=mask&4;
  p.admit=mask&8;p.grade=mask&16;p.perceptualReady=mask&32;p.m_svdLutRawTest.activationPts=mask&64?0:INVALID_TIME;
  const bool enabled=(mask&385)&&!(mask&6), lut=enabled&&(mask&1)&&((mask&8)||(mask&64)), skin=enabled&&!(lut&&p.grade);
  if(legacy)p.DrawSvdLut(nullptr,{},0,true);else{auto hr=p.DrawCompletedSvdLut({},nullptr,{},false);assert(hr==((lut&&p.grade)||(skin&&p.perceptualReady)?S_OK:S_FALSE));}
  assert(p.raw==int(lut)&&p.skin==int(skin));++cases;
 }}
 std::cout<<"PASS "<<cases<<" extracted completed/legacy draw routing cases\n";
}
'''
# Resource and pressure contracts checked separately; not compilation of D3D11 code.
assert 'prepare(shaders.standalone' not in s
assert 'StandalonePerceptualBytecode()' in raw
assert 'ready.store(true, std::memory_order_release)' in s
assert 'job->ready.load(std::memory_order_acquire)' in s
# Both fused and calibrated fallback bind full strength, only after a valid LUT.
assert 'VideoEnhancementFade' not in s and 'VideoEnhancementFade' not in raw
fused=method('HRESULT CDX11VideoProcessor::ApplyPerceptual(')
assert fused.index('(mode == 2 && !lut)') < fused.index('0.01f * m_pFilter->m_videoEnhancement.load()/100.f, 1.0f,')
fallback=raw[raw.index('HRESULT CDX11VideoProcessor::ApplySvdLutRawFrame('):]
assert fallback.index('if (!lut)') < fallback.index('m_pFilter->m_videoEnhancement.load()/100.f, float(offset.x), float(offset.y), 0.0f,')
assert 'b.activationPts = pts;' in raw # first-present/legacy admission bookkeeping retained
assert s.index('(m_completedPresentFairness.Active() || !VideoEnhancementMayStart(true))') < s.index('const auto drawn = DrawCompletedVideo(frame, m_svdLut.picture')
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp';p.write_text(code);e=Path(d)/'test'
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-O2',str(p),'-o',str(e)],check=True)
    subprocess.run([str(e)],check=True)
print('PASS independent bytecode publication and scaled fallback guard (static)')
