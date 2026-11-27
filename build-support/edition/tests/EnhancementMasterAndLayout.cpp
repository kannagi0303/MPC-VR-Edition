// SPDX-License-Identifier: GPL-3.0-or-later
// Production method fragments are supplied by the Edition integration runner.
#undef NDEBUG
#include <atomic>
#include <cassert>
#include <cstring>
#include <optional>
#include <iostream>
#include <memory>

namespace {
using HRESULT=int;using LPCSTR=const char*;
constexpr int S_OK=0,S_FALSE=1,E_INVALIDARG=-1,State_Paused=1;
struct CAutoLock{CAutoLock(int*){}};
struct Base{virtual ~Base()=default;};
struct CDX11VideoProcessor:Base{int reset=0,release=0,invalidate=0;void ResetAdaptiveTone(){++reset;}void ReleaseSvdLutRawTest(bool){++release;}void InvalidateCompletedBackBuffer(){++invalidate;}};
struct CMpcVideoRenderer{
 int m_RendererLock=0,m_filterState=State_Paused,redraw=0;
 std::atomic_int m_videoEnhancement{100},m_memoryColor{100},m_skinTone{100},m_adaptiveTone{100};
 bool m_enhancementEnabled=true; int m_requestedColorStrengths[4]{100,100,100,100};
 std::unique_ptr<Base> m_VideoProcessor=std::make_unique<CDX11VideoProcessor>();
 void Redraw(){++redraw;}void ApplyEnhancementStrengths();
 std::optional<HRESULT> SetEditionInt(LPCSTR,int),GetEditionInt(LPCSTR,int*),SetEditionBool(LPCSTR,bool),GetEditionBool(LPCSTR,bool*);
};

#include "EnhancementControlMethods.generated.inc"

void Exercise(){
 constexpr const char* keys[]={"colorEnhancementStrength","memoryColorStrength","skinToneStrength","adaptiveToneStrength"};
 unsigned cases=0;
 for(int i=0;i<101;++i) for(int mask=0;mask<16;++mask){
  CMpcVideoRenderer r;auto* dx=static_cast<CDX11VideoProcessor*>(r.m_VideoProcessor.get());
  std::atomic_int* eff[]={&r.m_videoEnhancement,&r.m_memoryColor,&r.m_skinTone,&r.m_adaptiveTone};
  int want[4];for(int j=0;j<4;++j){want[j]=(mask&(1<<j))?i:100;r.SetEditionInt(keys[j],want[j]);}
  int rel=dx->release,reset=dx->reset,invalid=dx->invalidate;
  assert(r.SetEditionBool("enhancementEnabled",false)==S_OK);
  bool enabled=true;assert(r.GetEditionBool("enhancementEnabled",&enabled)==S_OK&&!enabled);
  for(int j=0;j<4;++j){int x=-1;assert(eff[j]->load()==0&&r.GetEditionInt(keys[j],&x)==S_OK&&x==want[j]);}
  assert(dx->release==rel+(want[0]!=0)&&dx->reset==reset+(want[3]!=0));
  assert(dx->invalidate==invalid+bool(want[0]||want[1]||want[2]||want[3]));
  invalid=dx->invalidate;
  assert(r.SetEditionInt(keys[1],42).value()>=0);want[1]=42;
  assert(dx->invalidate==invalid&&eff[1]->load()==0);
  assert(r.SetEditionBool("enhancementEnabled",true)==S_OK);
  for(int j=0;j<4;++j)assert(eff[j]->load()==want[j]);
  assert(r.SetEditionInt(keys[0],101)==E_INVALIDARG&&r.SetEditionInt(keys[0],-1)==E_INVALIDARG);
  // Retired experimental far-dimming key is not part of the four-control master.
  assert(!r.SetEditionInt("farDimmingStrength",50).has_value());
  assert(r.SetEditionBool("enhancementEnabled",true)==S_FALSE);++cases;
 }
 std::cout<<"PASS "<<cases<<" extracted master/strength preservation and zero-crossing cases\n";
}
}

void TestEnhancementMasterAndLayout() { Exercise(); }
