"""Portable checks of actual false-color ramp and RAW consumer admission.
No GPU rendering, DirectML throughput or Windows playback claims.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
overlay = (root/'Source/Edition/DX11DepthOverlay.cpp').read_text()
cpp = (root/'Source/Depth/Depth.cpp').read_text()
body = cpp[cpp.index('bool CDepth::DrawEstimate('):]
gate = body[body.index('    PollCompleted();'):body.index('    D3D11_TEXTURE2D_DESC desc{};')]
assert 'PollCompleted();' in gate
assert gate.index('PollCompleted();') < gate.index('EstimateAtOrBefore(pts)')
assert body.count('context->Draw(3, 0)') == 1
assert 'm_temporalShader' not in body and 'm_grids' not in body
shader = overlay[overlay.index('    // Display-only false color:'):overlay.index('    return float4(saturate(color), 1.0);')]
# Read actual HLSL palette stops; check interpolation stays within gamut and is
# continuous. This is not shader compilation or visual acceptance.
lines = [l for l in shader.splitlines() if 'color = lerp(' in l]
assert len(lines) == 6
stops = [0.0] + [float(re.search(r'z < ([\d.]+)', l).group(1)) for l in lines[:-1]] + [1.0]
colors = [tuple(map(float, re.search(r'lerp\(float3\(([^)]+)\)', l).group(1).split(','))) for l in lines]
ends = [tuple(map(float, re.search(r'\), float3\(([^)]+)\)', l).group(1).split(','))) for l in lines]
assert colors[1:] == ends[:-1]
assert colors[0][2] > colors[0][0] > colors[0][1]
assert ends[-1][0] > ends[-1][1] > ends[-1][2] or ends[-1][0] > ends[-1][1] == ends[-1][2]
for i, (a, b) in enumerate(zip(colors, ends)):
    assert stops[i+1] > stops[i]
    for j in range(1001):
        t = j/1000
        assert all(0 <= x+(y-x)*t <= 1.000001 for x,y in zip(a,b))
assert 'const float z = saturate(0.5 + (0.5 - depth) / DepthSpan);' in shader

program = r'''
#include "DepthRawPolicy.h"
#include <cassert>
#include <cstdint>
#include <string_view>
#include <iostream>
#include <random>
using std::int64_t;
struct CDepth {
    static constexpr int64_t MaxEstimateAge = 2000000;
    struct Estimate { bool view; int64_t pts; };
    Estimate map{false,-1}, ready{false,-1};
    int64_t scene=-1;
    bool m_depthShader=true;
    struct Counters { int64_t rawMfgCheckAge=-1; const wchar_t* rawMfgCheck=L"not checked"; } m_counters;
    void PollCompleted() { if(ready.view) { map=ready; ready={false,-1}; } }
    Estimate EstimateAtOrBefore(int64_t pts) const { return map.view && map.pts<=pts ? map : Estimate{false,-1}; }
    int64_t SceneStart(int64_t) const { return scene; }
bool Check(int64_t pts,float frameTimeMs,bool inverted) {
''' + gate + r'''
    record(L"used");
    return true;
}
};
int main(){
    using std::wstring_view;
    CDepth a;
    a.ready={true,0};
    assert(a.Check(400000,40,true));
    assert(a.m_counters.rawMfgCheckAge==400000);
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"used");
    assert(!a.Check(800000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"PTS mismatch");
    a.scene=1;
    assert(!a.Check(400000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"scene mismatch");
    a.scene=-1;
    assert(!a.Check(3000000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"too old");
    a.m_depthShader=false;
    assert(!a.Check(400000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"shader unavailable");
    a.map={true,500000};
    assert(!a.Check(400000,40,true));
    assert(wstring_view(a.m_counters.rawMfgCheck)==L"no map");
    assert(a.m_counters.rawMfgCheckAge==-1);
    std::mt19937 rng(26);
    for(int i=0;i<100000;++i){
        CDepth b;
        b.map={bool(rng()%2),int64_t(rng()%3000000)};
        b.scene=int64_t(rng()%3000000)-1;
        b.m_depthShader=bool(rng()%2);
        int64_t pts=rng()%3000000;
        float frameTimeMs=float(rng()%100);
        const auto e=b.EstimateAtOrBefore(pts);
        bool prior=e.view && e.pts>=b.scene && pts-e.pts<=CDepth::MaxEstimateAge && b.m_depthShader &&
            depthraw::Usable(e.pts,pts,frameTimeMs);
        assert(b.Check(pts,frameTimeMs,true)==prior);
    }
    std::cout<<"PASS: late harvest, RAW rejection reasons, 100000 equivalent eligibility cases\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'test.cpp'; p.write_text(program)
    exe=Path(d)/'test'
    subprocess.run([os.getenv('CXX','clang++'),'-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root/'Source/Depth'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: six continuous false-color preview segments, no changes to model output')
