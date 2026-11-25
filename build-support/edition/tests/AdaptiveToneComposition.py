# SPDX-License-Identifier: GPL-3.0-or-later
# [FA-01 | 2026-11-25 14:07 +08:00] Extract actual fused scalar shader math.
# Portable C++ surrogate for HLSL arithmetic, not a Windows shader/GPU test.
from pathlib import Path
import subprocess, tempfile, os
root=Path(__file__).resolve().parents[3]
s=(root/'Source/DLSS/PerceptualColorShader.h').read_text()
a=(root/'Source/Edition/AdaptiveToneShader.h').read_text()
def function(text,name):
    begin=text.index(name); start=text.index('{',begin); n=1; end=start+1
    while n:
        n+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]
assert s.index('ApplyB22Chroma(original, grade)') < s.index('float3 toned = ApplyAdaptiveTone(grade') < s.index('float3 enhanced = Enhance(original, toned')
main=function(s,'float4 SvdLutGradeMain(')
tail=main[main.index('    float3 toned ='):]
fade=main[main.index('    float activation = saturate(DepthTuning.w);'):main.index('#endif\n    if (Enhancements.x') ]
assert 'lerp(toneOnly, enhanced' not in main
code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
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
float saturate(float a){return std::clamp(a,0.f,1.f);} float3 saturate(float3 a){return {saturate(a.r),saturate(a.g),saturate(a.b)};}
float lerp(float a,float b,float w){return a+(b-a)*w;} float3 lerp(float3 a,float3 b,float3 w){return a+(b-a)*w;}
float3 step(float a,float3 b){return {float(b.r>=a),float(b.g>=a),float(b.b>=a)};}
const float3 YW(.2126,.7152,.0722);
float4 Enhancements(1,1,1,1), OutputQuantization, DepthTuning;
std::array<float,257> curve;
float ATValue(uint i){return curve[i];}
'''
code+=function(s,'float Smooth(')+'\n'+function(s,'float3 Enhance(')+'\n'+function(a,'float3 ApplyAdaptiveTone(')+'\n'
code+='float4 Compose(float3 original,float3 grade,float depthWeight,float farWeight){ struct {float a=1;} src;\n'+fade+tail+'\n'
code+=r'''
float err(float3 a,float3 b){return length(a-b);}
float3 rgb(float4 a){return {a.x,a.y,a.z};}
int main(){
    for(uint i=0;i<=256;++i){float x=i/256.f;curve[i]=x+.10f*4*x*(1-x);}
    std::mt19937 r(523);std::uniform_real_distribution<float> u(0,1);
    unsigned changed=0;
    for(int i=0;i<100000;++i){
        float3 orig(u(r),u(r),u(r)),grade(u(r),u(r),u(r));float depth=.9f+.2f*u(r),far=u(r),v=u(r),w=u(r);
        auto reference=[&](float3 input,float weight,float farWeight){
            auto result=Enhance(orig,ApplyAdaptiveTone(input,weight),depth);
            if(farWeight>.001f){float y=dot(result,YW);float darken=min(DepthTuning.z,.02)*farWeight*(1.f-Smooth(.80,.98,y));float3 srgbSlope=lerp(result,(result+.055)/2.4,step(.04045,result));result=saturate(result-darken*srgbSlope);}
            return result;
        };
        DepthTuning=float4(0,0,.01f,1);OutputQuantization.y=w;
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
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-O2',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
