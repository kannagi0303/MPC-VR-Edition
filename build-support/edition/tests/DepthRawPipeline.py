"""Focused portable check of the DP2 raw policy and actual scalar shader math.
Does not execute HLSL, D3D11, DirectML, or prove GPU scheduling/visual quality.
Run with Python 3 and a C++20 compiler (g++/clang++).
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
DEPTH = ROOT / 'Source/Depth'
shader = (DEPTH / 'DepthGpuShaders.h').read_text(encoding='utf-8')
consumer = (DEPTH / 'DepthDrawShader.h').read_text()
# Extract the exact scalar HLSL expressions used by production.
scalar = shader[shader.index('float2 RawEmpty()'):shader.index('[numthreads(256, 1, 1)]\nvoid RawRangeTiles')]
finite = re.search(r'bool FiniteDepth\(float value\) \{.*?\n\}', shader, re.S).group(0)
expr = re.search(r'void RawNormalize\(.*?Depth\[pixel\] = (.*?);\n\}', shader, re.S).group(1)
raw_expand = consumer[consumer.index('float RawExpand('):]
expand_expr = re.search(r'return (0\.5 \+ \(depth - 0\.5\).*?);', raw_expand).group(1)
assert all(x not in raw_expand for x in ['History.Sample', 'Motion.Load', 'CompareLuma', 'Stabilize('])
cpp = (DEPTH / 'Depth.cpp').read_text()
raw_draw = cpp[cpp.index('bool CDepth::DrawEstimate('):]
assert 'm_grids' not in raw_draw and 'm_temporalShader' not in raw_draw and raw_draw.count('context->Draw(3, 0)') == 1
worker = (DEPTH / 'DepthGpu.cpp').read_text(encoding='utf-8-sig')
assert 'directory / depthsplit::GraphFileName' in worker and 'directory / depthsplit::WeightFile' in worker
assert 'rangePipeline = Pipeline("RawRange")' in worker and 'Pipeline("Percentiles")' not in worker
assert 'UavBarrier(postList.Get(), range.Get());\n            postList->SetPipelineState(rangePipeline.Get());' in worker
mfg = (ROOT / 'Source/DX11VideoProcessorMfg.cpp').read_text()
assert 'if (DepthOverlayEnabled())' in mfg
# Use double literals consistently with HLSL single precision in scalar port.
scalar = scalar.replace('0.0', '0.0f')
expr = re.sub(r'(?<![\w.])(0\.56|0\.12|0\.5)(?![\w.])', r'\1f', expr)
expand_expr = re.sub(r'(?<![\w.])(0\.12|0\.5)(?![\w.])', r'\1f', expand_expr)
program = r'''
#include "DepthRawPolicy.h"
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <random>
#include <iostream>
using std::min; using std::max;
using uint=std::uint32_t;
struct float2 { float x,y; };
struct float4 { float x,y,z,w; };
uint asuint(float v) { return std::bit_cast<uint>(v); }
float asfloat(uint v) { return std::bit_cast<float>(v); }
''' + finite + '\n' + scalar + r'''
float Normalize(float v, float lo, float hi, uint flags) {
    uint Range[3]{0,0,flags};
    return ''' + expr + r''';
}
float Expand(float depth, bool inverted) {
    if (inverted) depth=1.0f-depth;
    const float4 DepthSettings{0,0,0,0.30f};
    return ''' + expand_expr + r''';
}
int main() {
    using depthraw::Usable;
    assert(Usable(0,0,0));
    assert(!Usable(-1,0,42));
    assert(!Usable(100,99,42));
    assert(Usable(100,400100,40));
    assert(!Usable(100,800100,40));
    assert(!Usable(100,101,std::numeric_limits<float>::infinity()));
    assert(!Usable(100,101,0));
    std::mt19937 rng(26);
    for (unsigned n : {67584u,307200u,1025u,1u}) {
        std::vector<float> pixels(n);
        for (auto& x : pixels) x = float(rng()%100000)/1000;
        pixels[0]=0; pixels[n-1]=200;
        auto global=RawEmpty();
        // Mirror tile dispatch and reduce all pixels through extracted math.
        for (unsigned tile=0;tile<(n+1023)/1024;++tile) {
            auto bounds=RawEmpty();
            for (unsigned lane=0;lane<256;++lane)
                for (unsigned i=0;i<4;++i) {
                    auto p=tile*1024+lane+i*256;
                    if(p<n) bounds=RawMerge(bounds,RawSample(pixels[p]));
                }
            global=RawMerge(global,bounds);
        }
        auto [lo,hi]=std::minmax_element(pixels.begin(),pixels.end());
        assert(global.x==*lo && global.y==*hi);
        for (float v : pixels) {
            const float z=Normalize(v,global.x,global.y,1);
            assert(z>=0.4399999f && z<=0.5600001f);
            assert(std::abs(Expand(z,false)+Expand(z,true)-1)<1e-6f);
        }
    }
    assert(std::abs(Normalize(0,0,4,1)-0.56f)<1e-6f);
    assert(std::abs(Normalize(4,0,4,1)-0.44f)<1e-6f);
    assert(Normalize(1,1,1,1)==0.5f);
    assert(Normalize(0,0,1,0)==0.5f);
    assert(Normalize(1,0,1,3)==Normalize(1,0,1,1)); // cut bit doesn't change depth
    for(float v : {-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        assert(!FiniteDepth(RawMerge(RawSample(1),RawSample(v)).y));
    assert(std::abs(Expand(0.44f,false)-0.35f)<1e-6f);
    assert(std::abs(Expand(0.56f,true)-0.35f)<1e-6f);
    std::cout << "PASS: fixed depth paths, PTS guards, full-map reduction (375810 samples), polarity, flat/invalid guards, consumer mapping.\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td)/'raw.cpp'; p.write_text(program)
    exe=Path(td)/'raw'
    subprocess.run([os.getenv('CXX','g++'), '-std=c++20','-O1','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(DEPTH),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
