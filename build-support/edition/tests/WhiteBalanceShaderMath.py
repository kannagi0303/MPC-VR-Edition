# SPDX-License-Identifier: GPL-3.0-or-later
# Extract the production WB HLSL function into a scalar C++ harness. No GPU claim.
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[3]
s=(root/'Source/Edition/WhiteBalanceShader.h').read_text().split('R"(',1)[1].split(')";',1)[0]
s=s.replace('control.xyz','float3(control.x,control.y,control.z)')
code=r'''
#include "Edition/WhiteBalance.h"
#include <random>
#include <cassert>
#include <iostream>
struct float3 { float r,g,b; float3(float a):r(a),g(a),b(a){} float3(float r,float g,float b):r(r),g(g),b(b){} };
struct float4 {float x,y,z,w;};
#define OP(o) float3 operator o(float3 a,float3 b){return {a.r o b.r,a.g o b.g,a.b o b.b};}
OP(+) OP(-) OP(*) OP(/)
#undef OP
float max(float a,float b){return std::max(a,b);}float min(float a,float b){return std::min(a,b);}
float saturate(float a){return std::clamp(a,0.f,1.f);}float3 saturate(float3 a){return {saturate(a.r),saturate(a.g),saturate(a.b)};}
float dot(float3 a,float3 b){return a.r*b.r+a.g*b.g+a.b*b.b;}
float3 pow(float3 a,float b){return {std::pow(a.r,b),std::pow(a.g,b),std::pow(a.b,b)};}
float3 step(float a,float3 b){return {float(b.r>=a),float(b.g>=a),float(b.b>=a)};}
float3 lerp(float3 a,float3 b,float3 t){return a+(b-a)*t;}
'''+s+r'''
int main(){using namespace edition::white_balance;std::mt19937 r(222);std::uniform_real_distribution<float> u(0,1),v(-1,1);float maxError=0;
 for(int i=0;i<100000;++i){RGB rgb{u(r),u(r),u(r)},cast{v(r),v(r),v(r)};auto g=Gains(cast,i%101);auto a=Correct(rgb,g);auto b=ApplyWhiteBalance(float3(rgb[0],rgb[1],rgb[2]),{g[0],g[1],g[2],g==Identity?0.f:1.f});
 for(auto [x,y]:{std::pair(a[0],b.r),std::pair(a[1],b.g),std::pair(a[2],b.b)}){assert(std::isfinite(y)&&y>=0&&y<=1);maxError=std::max(maxError,std::abs(x-y));}}
 assert(maxError<1e-5f);std::cout<<"PASS 100000 extracted WB shader probes; CPU max difference "<<maxError<<'\n';}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d); (p/'test.cpp').write_text(code)
 subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-O2','-I',str(root/'Source'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
# Static integration checks. The same committed epoch is passed to inference
# thumbnail, fused VE and independent colour draw, and reset before new captures.
ve=(root/'Source/DX11VideoProcessorSvdLut.cpp').read_text(); at=(root/'Source/Edition/DX11AdaptiveTone.cpp').read_text()
assert ve.count('const auto wb=WhiteBalanceControl();')==2
assert 'const auto wb=WhiteBalanceControl();' in at
assert 'a.wbGain=gain;' in at and 'ResetSvdLutTemporalQueue();' in at
assert 'if(wb && !priming)a.wbDeferred.Consume(a.wb,pts);' in at
assert 'if(defer)a.wbDeferred.Push(sample,estimate,a.capturePts);' in at
assert 'if(wb && !priming)SyncWhiteBalance();' in at
assert 'if (!drawShader || WhiteBalanceActive()) return S_FALSE;' in (root/'Source/Edition/SvdLutRawMode.inc').read_text()
print('PASS WB epoch, fallback, original capture and independent control wiring (static)')
