// SPDX-License-Identifier: GPL-3.0-or-later
// Developer-only scalar harness. SvdLutAdaptiveChromaModel.py extracts the
// exact HLSL helper body into the temporary include; no copied algorithm.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>
struct float3 { float r,g,b; float3(float x,float y,float z):r(x),g(y),b(z){} };
float3 operator-(float3 a,float3 b){return {a.r-b.r,a.g-b.g,a.b-b.b};}
float3 operator-(float3 a,float b){return {a.r-b,a.g-b,a.b-b};}
float3 operator+(float3 a,float3 b){return {a.r+b.r,a.g+b.g,a.b+b.b};}
float3 operator+(float a,float3 b){return {a+b.r,a+b.g,a+b.b};}
float3 operator*(float3 a,float b){return {a.r*b,a.g*b,a.b*b};}
float dot(float3 a,float3 b){return a.r*b.r+a.g*b.g+a.b*b.b;}
float max(float a,float b){return std::max(a,b);}
float min(float a,float b){return std::min(a,b);}
float clamp(float a,float lo,float hi){return std::clamp(a,lo,hi);}
float saturate(float a){return std::clamp(a,0.f,1.f);}
float3 saturate(float3 a){return {saturate(a.r),saturate(a.g),saturate(a.b)};}
float3 abs(float3 a){return {std::abs(a.r),std::abs(a.g),std::abs(a.b)};}
#include "SvdLutAdaptiveChromaShader.inc"
float radius(float3 c){c=abs(c);return max(max(c.r,c.g),c.b);}
float y(float3 c){return dot(c,float3(.2126f,.7152f,.0722f));}
float3 anchor(float3 s,float l){
    auto c=s-y(s);float hi=max(max(c.r,c.g),max(c.b,1e-6f));
    float lo=max(max(-c.r,-c.g),max(-c.b,1e-6f));
    return l+c*saturate(min((1-l)/hi,l/lo));
}
int main(int argc,char** argv){
    static_assert(sizeof(float3)==12);
    if(argc==3){
        std::ifstream in(argv[1],std::ios::binary);std::ofstream out(argv[2],std::ios::binary);
        if(!in||!out)return 2;
        float a[6];while(in.read(reinterpret_cast<char*>(a),sizeof(a))){
            auto c=ApplyB22Chroma({a[0],a[1],a[2]},{a[3],a[4],a[5]});
            out.write(reinterpret_cast<const char*>(&c),sizeof(c));
        }
        return in.eof()&&in.gcount()==0&&out?0:3;
    }
    std::mt19937 rng(122);std::uniform_real_distribution<float> u(0,1);
    float maxY=0;
    for(int i=0;i<100000;++i){
        float3 s(u(rng),u(rng),u(rng)),g(u(rng),u(rng),u(rng));
        auto b=ApplyB21Luma(s,g,.25f*u(rng));auto r=ApplyB22Chroma(s,b);
        for(float v:{r.r,r.g,r.b})assert(v>=0&&v<=1);
        maxY=max(maxY,std::abs(y(b)-y(r)));assert(std::abs(y(b)-y(r))<4e-7f);
        const auto a=anchor(s,saturate(y(b)));float lim=2.f/255+1.5f*radius(s-y(s));
        assert(radius(r-a)<=lim+4e-7f);
        if(radius(b-a)<=.65f*lim)assert(radius(r-b)==0);
        assert(radius(ApplyB22Chroma(s,s)-s)<4e-7f);
    }
    for(int i=0;i<=255;++i){
        float n=i/255.f;float3 s(n,n,n);float3 g(u(rng),u(rng),u(rng));
        auto r=ApplyB22Chroma(s,ApplyB21Luma(s,g,.25f));assert(radius(r-y(r))<=2.f/255+4e-7f);
    }
    // Fixed source and ray: soft-knee must remain ordered, not a plateau.
    float prev=-1;float3 s(.5,.5,.5);
    for(int i=0;i<=4096;++i){
        float t=.49f*i/4096;float3 b(.5f+t,.5f-t*.2126f/.7152f,.5f);
        float v=radius(ApplyB22Chroma(s,b)-s);assert(v>=prev-1e-7f);prev=v;
    }
    std::cout<<"PASS 100000 random cases; 256 neutral inputs; 4097 fixed-ray probes; max code-luma error="<<maxY*255<<"\n";
}
