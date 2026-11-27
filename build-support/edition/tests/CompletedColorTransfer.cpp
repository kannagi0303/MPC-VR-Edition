// SPDX-License-Identifier: GPL-3.0-or-later
// Portable ownership mock. The integration runner extracts the production helper.
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <deque>
#include <vector>
#include <array>
#include <utility>
namespace {
using UINT=unsigned;
constexpr int DXGI_FORMAT_B8G8R8A8_UNORM=87,D3D11_USAGE_DEFAULT=0,D3D11_BIND_SHADER_RESOURCE=8,D3D11_BIND_RENDER_TARGET=32,Tex2D_DefaultShaderRTarget=4;
struct CRect{long l,t,r,b; CRect(long a,long c,long d,long e):l(a),t(c),r(d),b(e){} bool operator==(const CRect&)const=default;};
struct ID3D11Texture2D{unsigned id; std::array<int,8> pixels{};};
struct View{std::shared_ptr<ID3D11Texture2D> owner;};
template<class T> struct Ptr{std::shared_ptr<T> owned;T* p=nullptr; Ptr()=default; explicit Ptr(std::shared_ptr<T> a):owned(std::move(a)),p(owned.get()){} explicit operator bool()const{return p!=nullptr;} };
struct Desc{UINT Width=3840,Height=2160,MipLevels=1,ArraySize=1;int Format=87;struct{UINT Count=1,Quality=0;}SampleDesc;int Usage=0;UINT BindFlags=40,CPUAccessFlags=0,MiscFlags=0;};
struct Tex2D_t{Ptr<ID3D11Texture2D> pTexture;Desc desc;Ptr<View> pShaderResource;int type=4;};
Tex2D_t Make(unsigned id){Tex2D_t t;auto tex=std::make_shared<ID3D11Texture2D>();tex->id=id;t.pTexture=Ptr<ID3D11Texture2D>(tex);t.pShaderResource=Ptr<View>(std::make_shared<View>(View{tex}));return t;}
#include "CompletedColorTransfer.generated.inc"
struct Frame{Tex2D_t image; long pts;uint64_t fence=0,readFence=0;};
void CheckCoherent(const Tex2D_t& t){assert(t.pTexture&&t.pShaderResource&&t.pTexture.p==t.pShaderResource.p->owner.get());}
void Exercise(){
 unsigned cases=0;
 // Every individual invalid descriptor/identity and pair of invalid conditions must retain both wrappers.
 for(unsigned first=0;first<=35;++first)for(unsigned second=0;second<=35;++second){
  auto a=Make(1),b=Make(2),foreign=Make(3);CRect rect{0,0,3840,2160};bool video=true;uint64_t fence=0,readFence=0;auto* corrected=b.pTexture.p;auto* render=a.pTexture.p;
  auto change=[&](unsigned i){switch(i){case 0:break;case 1:video=false;break;case 2:fence=1;break;case 3:readFence=2;break;case 4:a.pTexture={};break;case 5:b.pTexture={};break;case 6:a.pShaderResource={};break;case 7:b.pShaderResource={};break;case 8:corrected=foreign.pTexture.p;break;case 9:render=foreign.pTexture.p;break;case 10:a.pTexture=b.pTexture;render=b.pTexture.p;break;case 11:a.type=0;break;case 12:b.type=0;break;case 13:a.desc.Format=28;break;case 14:b.desc.Format=28;break;case 15:a.desc.Width=0;break;case 16:a.desc.Height=0;break;case 17:b.desc.Width=1920;break;case 18:b.desc.Height=1080;break;case 19:rect.r=3839;break;case 20:a.desc.MipLevels=2;break;case 21:b.desc.MipLevels=2;break;case 22:a.desc.ArraySize=2;break;case 23:b.desc.ArraySize=2;break;case 24:a.desc.SampleDesc.Count=2;break;case 25:b.desc.SampleDesc.Count=2;break;case 26:a.desc.SampleDesc.Quality=1;break;case 27:b.desc.SampleDesc.Quality=1;break;case 28:a.desc.Usage=1;break;case 29:b.desc.Usage=1;break;case 30:a.desc.BindFlags=0;break;case 31:b.desc.BindFlags=0;break;case 32:a.desc.CPUAccessFlags=1;break;case 33:b.desc.CPUAccessFlags=1;break;case 34:a.desc.MiscFlags=1;break;case 35:b.desc.MiscFlags=1;break;}};
  change(first);change(second);auto* ap=a.pTexture.p;auto* bp=b.pTexture.p;auto* av=a.pShaderResource.p;auto* bv=b.pShaderResource.p;
  bool ok=TryAdoptCompletedColorOutput(a,b,corrected,render,rect,video,fence,readFence);
  assert(ok==(first==0&&second==0));assert(a.pTexture.p==(ok?bp:ap)&&b.pTexture.p==(ok?ap:bp));assert(a.pShaderResource.p==(ok?bv:av)&&b.pShaderResource.p==(ok?av:bv));++cases;
 }
 // Simulate a serial producer with delayed queue composition/presentation,
 // including pending frames, recycling, raw/no-grade and ineligible copy paths.
 // The GPU itself is NOT simulated; pixel arrays only check ownership isolation.
 auto scratch=Make(0);std::deque<Frame> pending,ready;std::vector<Tex2D_t> free;unsigned next=1,adoptions=0,copies=0,raw=0;
 for(int i=1;i<=20000;++i){
  Tex2D_t input;if(free.empty()) input=Make(next++);else{input=std::move(free.back());free.pop_back();}
  CheckCoherent(input);CheckCoherent(scratch);input.pTexture.p->pixels.fill(i);const auto old=input.pTexture.owned;
  int expected=i;bool grade=i%7!=0;bool full=i%5!=0;
  if(grade){scratch.pTexture.p->pixels.fill(-i);expected=-i;auto* corrected=scratch.pTexture.p;
   if(TryAdoptCompletedColorOutput(input,scratch,corrected,old.get(),{0,0,full?3840:3839,2160},true,0,0)){++adoptions;assert(scratch.pTexture.p==old.get());}
   else{input.pTexture.p->pixels=corrected->pixels;++copies;assert(input.pTexture.p==old.get());}
  }else ++raw;
  CheckCoherent(input);CheckCoherent(scratch);assert(input.pTexture.p!=scratch.pTexture.p);assert(input.pTexture.p->pixels[0]==expected);
  pending.push_back({std::move(input),i});
  if(pending.size()>7){auto f=std::move(pending.front());pending.pop_front();f.fence=f.pts;ready.push_back(std::move(f));}
  if(ready.size()>120){auto f=std::move(ready.front());ready.pop_front();CheckCoherent(f.image);assert(f.image.pTexture.p->pixels[0]==(f.pts%7?-f.pts:f.pts));assert(f.fence==uint64_t(f.pts));free.push_back(std::move(f.image));}
  for(const auto& queue:{&pending,&ready})for(const auto& f:*queue){assert(f.image.pTexture.p!=scratch.pTexture.p);assert(f.image.pTexture.p->pixels[0]==(f.pts%7?-f.pts:f.pts));}
 }
 assert(adoptions+copies+raw==20000);std::cout<<"PASS "<<cases<<" descriptor/identity cases; 20000 ownership turns, "<<adoptions<<" adopts, "<<copies<<" copy fallback, "<<raw<<" raw\n";
}

} // namespace
void TestCompletedColorTransfer(){Exercise();}
#ifdef MPCVR_STANDALONE_TEST
int main(){TestCompletedColorTransfer();}
#endif
