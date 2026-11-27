// SPDX-License-Identifier: GPL-3.0-or-later
// Execute production scalar HLSL in a CPU fixture for numerical regression checks.
#undef NDEBUG
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
namespace {
using uint=unsigned;
struct float3 { float r,g,b; float3(float a=0):r(a),g(a),b(a){} float3(float x,float y,float z):r(x),g(y),b(z){} };
struct float4 {float x,y,z,w; float4(float a=0,float b=0,float c=0,float d=0):x(a),y(b),z(c),w(d){} float4(float3 a,float d):x(a.r),y(a.g),z(a.b),w(d){} };
#define OP(o) float3 operator o(float3 a,float3 b){return {a.r o b.r,a.g o b.g,a.b o b.b};}
OP(+) OP(-) OP(*) OP(/)
#undef OP
float3& operator*=(float3& a,float3 b){return a=a*b;} float3& operator-=(float3& a,float3 b){return a=a-b;}
float3 operator-(float3 a){return 0.f-a;}
float min(float a,float b){return std::min(a,b);} uint min(uint a,uint b){return std::min(a,b);}
float max(float a,float b){return std::max(a,b);}
float3 max(float3 a,float3 b){return {max(a.r,b.r),max(a.g,b.g),max(a.b,b.b)};}
float dot(float3 a,float3 b){return a.r*b.r+a.g*b.g+a.b*b.b;}
float length(float3 a){return std::sqrt(dot(a,a));} float rsqrt(float a){return 1.f/std::sqrt(a);}
float clamp(float a,float lo,float hi){return std::clamp(a,lo,hi);}
float saturate(float a){return std::clamp(a,0.f,1.f);} float3 saturate(float3 a){return {saturate(a.r),saturate(a.g),saturate(a.b)};}
float lerp(float a,float b,float w){return a+(b-a)*w;} float3 lerp(float3 a,float3 b,float3 w){return a+(b-a)*w;}
float3 step(float a,float3 b){return {float(b.r>=a),float(b.g>=a),float(b.b>=a)};}
const float3 YW(.2126,.7152,.0722);
float4 Enhancements(1,1,1,1), OutputQuantization, DepthTuning;
std::array<float,257> curve;
float ATValue(uint i){return curve[i];}

#include "AdaptiveToneComposition.generated.inc"

float err(float3 a,float3 b){return length(a-b);}
float3 rgb(float4 a){return {a.x,a.y,a.z};}
void Exercise(){
    for(uint i=0;i<=256;++i){float x=i/256.f;curve[i]=x+.10f*4*x*(1-x);}
    std::mt19937 r(523);std::uniform_real_distribution<float> u(0,1);
    unsigned changed=0;
    for(int i=0;i<100000;++i){
        float3 orig(u(r),u(r),u(r)),grade(u(r),u(r),u(r));float depth=.9f+.2f*u(r),far=u(r),v=u(r),w=u(r);
        auto reference=[&](float3 input,float weight,float farWeight){
            auto result=Enhance(orig,ApplyAdaptiveTone(input,weight),depth);
            const float protection=1.f-.65f*Smooth(.80f,.98f,dot(result,YW));
            result *= float(std::pow(1.0-double(std::clamp(DepthTuning.z,0.f,.95f))*farWeight*protection,1.0/2.2));
            return result;
        };
        DepthTuning=float4(0,0,.50f*u(r),1);OutputQuantization.y=w;
        auto full=rgb(Compose(orig,grade,depth,far));
        assert(err(full,reference(grade,w,far))<1e-6f);
        DepthTuning.w=0;
        auto zero=rgb(Compose(orig,grade,depth,far));
        assert(err(zero,reference(orig,w,far))<1e-6f);
        if(err(zero,ApplyAdaptiveTone(orig,w))>1e-5f)++changed;
        DepthTuning.w=v;
        auto actual=rgb(Compose(orig,grade,depth,far));
        assert(err(actual,reference(lerp(orig,grade,v),w,far))<1e-6f);
        // At zero LUT strength, perceptual colour is already fully active.
        DepthTuning.w=0;OutputQuantization.y=0;
        assert(err(rgb(Compose(orig,grade,depth,0)),Enhance(orig,orig,depth))<1e-6f);
        for(float x:{actual.r,actual.g,actual.b})assert(x>=0&&x<=1);
    }
    // Independent controls: source-family weights, zero bypass, and low nonzero values.
    for(int i=0;i<100000;++i) {
        float3 orig(u(r),u(r),u(r)), grade(u(r),u(r),u(r));
        Enhancements=float4(0,0,1,1);
        assert(err(Enhance(orig,grade,1),grade)==0);
        assert(err(ApplyAdaptiveTone(orig,0),orig)==0);
        for(float x : {0.05f,.25f,.5f,1.f}) {
            Enhancements=float4(x,0,1,1);
            const auto mem=Enhance(orig,grade,1);
            Enhancements=float4(0,x,1,1);
            const auto skin=Enhance(orig,grade,1);
            for(float k:{mem.r,mem.g,mem.b,skin.r,skin.g,skin.b}) assert(k>=0&&k<=1);
        }
    }
    // The actual extracted shader composition must reduce linear light by
    // the requested fraction, including saturated highlights, with the current soft highlight protection.
    Enhancements=float4(0,0,0,0);OutputQuantization.y=0;
    for(float strength:{0.f,.01f,.10f,.25f,.50f}) {
        DepthTuning=float4(0,0,strength,1);
        for(float d:{.44f,.50f,.515f,.53f,.545f,.56f}) {
            const float f=Smooth(.50f,.56f,d);
            const float3 src(.95f,.5f,1.f);
            auto result=rgb(Compose(src,src,1,f));
            const float target=1-strength*f*(1.f-.65f*Smooth(.80f,.98f,dot(src,YW)));
            assert(std::abs(std::pow(result.r/src.r,2.2f)-target)<2e-6f);
            assert(std::abs(std::pow(result.g/src.g,2.2f)-target)<2e-6f);
            assert(std::abs(std::pow(result.b/src.b,2.2f)-target)<2e-6f);
            if(d<=.50f || strength==0) assert(err(result,src)==0);
        }
    }
    assert(std::abs(Smooth(.50f,.56f,.53f)-.5f)<2e-6f);
    assert(Smooth(.50f,.56f,.56f)==1.f);
    std::cout<<"PASS 30 linear-light depth/strength combinations, including 50% shader input and highlight protection\n";
    const float3 skinSrc(.65,.43,.35), plantSrc(.30,.60,.30);
    Enhancements=float4(0,1,1,1);
    assert(err(Enhance(skinSrc,skinSrc,1),skinSrc)>1e-5f);
    assert(err(Enhance(plantSrc,plantSrc,1),plantSrc)==0);
    Enhancements=float4(1,0,1,1);
    assert(err(Enhance(skinSrc,skinSrc,1),skinSrc)==0);
    assert(err(Enhance(plantSrc,plantSrc,1),plantSrc)>1e-5f);
    Enhancements=float4(0,.05f,1,1);
    assert(err(Enhance(skinSrc,skinSrc,1),skinSrc)>1e-6f);
    std::cout<<"PASS 100000 independent strength/bypass probes and skin/foliage examples\n";
    assert(changed>0); std::cout<<"PASS 100000 extracted composition probes: full-strength parity, zero LUT standalone, intermediate LUT-only fade; non-identity perceptual "<<changed<<'\n';
}
}

void TestAdaptiveToneComposition() { Exercise(); }
