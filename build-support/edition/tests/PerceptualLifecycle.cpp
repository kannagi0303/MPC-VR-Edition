// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <cassert>
#include <atomic>
#include <cstdint>
#include <iostream>

namespace {
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

#include "PerceptualMethods.generated.inc"

void Exercise(){unsigned cases=0;
 for(int mask=0;mask<512;++mask){for(bool legacy:{false,true}){
  CDX11VideoProcessor p; p.filter.m_videoEnhancement=mask&1;p.filter.m_memoryColor=mask&128;p.filter.m_skinTone=mask&256;p.filter.m_bFlushing=mask&2;p.m_fastShutdown=mask&4;
  p.admit=mask&8;p.grade=mask&16;p.perceptualReady=mask&32;p.m_svdLutRawTest.activationPts=mask&64?0:INVALID_TIME;
  const bool enabled=(mask&385)&&!(mask&6), lut=enabled&&(mask&1)&&((mask&8)||(mask&64)), skin=enabled&&!(lut&&p.grade);
  if(legacy)p.DrawSvdLut(nullptr,{},0,true);else{auto hr=p.DrawCompletedSvdLut({},nullptr,{},false);assert(hr==((lut&&p.grade)||(skin&&p.perceptualReady)?S_OK:S_FALSE));}
  assert(p.raw==int(lut)&&p.skin==int(skin));++cases;
 }}
 std::cout<<"PASS "<<cases<<" extracted completed/legacy draw routing cases\n";
}
}

void TestPerceptualLifecycle() { Exercise(); }
