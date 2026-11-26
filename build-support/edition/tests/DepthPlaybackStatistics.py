"""Portable test of actual depth publication-rate and accepted-use counters.
No GPU benchmark, no Windows execution or playback validation.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
header = (root / 'Source/Depth/Depth.h').read_text()
cpp = (root / 'Source/Depth/Depth.cpp').read_text()
sr = (root / 'Source/DLSS/DlssSR.cpp').read_text(encoding='utf-8-sig')
mfg = (root / 'Source/Mfg/Session.cpp').read_text()
osd = (root / 'Source/Edition/DX11Statistics.cpp').read_text()
# Extract production definitions rather than duplicating their math.
counters = header[header.index('    struct Counters {'):header.index('    static constexpr float OutputDepthSpan')]
methods = header[header.index('    void RecordSrEvaluation('):header.index('    bool CoolingDown(')]
rate = cpp[cpp.index('    // Wall-clock rate of valid maps'):cpp.index('\n}\n\nCDepth::Estimate CDepth::ForFrame')]
reset = cpp[cpp.index('void CDepth::ResetHistory()'):cpp.index('void CDepth::Stop(')]
assert 'm_mapRateTick = m_mapRateLastPoll = 0;' in reset
assert 'm_counters.mapRateSampleTick = 0;' in reset
assert sr.index('if (depthSource) depthSource->RecordSrEvaluation(depthActive);') > sr.index('if (NVSDK_NGX_FAILED(result))', sr.index('const auto result = NVSDK_NGX_D3D11_EvaluateFeature(ngxContext'))
assert mfg.index('if(bank.valid && depth && depth->applyDepth) depth->source.RecordMfgSubmission(depthActive);') > mfg.index('if(!SubmitBank(bank)) throw std::runtime_error("MFG GPU evaluate timed out");')
assert '++m_counters.maps;' in cpp[cpp.index('if (estimate.valid)'):cpp.index('    // Wall-clock rate')]
assert 'now - depth.mapRateSampleTick <= 2000' in osd
assert 'depth.gpuTimings.modelMs' in osd and '(pack/model/post)' in osd
assert 'Ready buffer {}%' in osd and 'Color Enhancement {}' in osd
assert osd.index('if (!showInformation) return std::nullopt;') < osd.index('const auto editionStart = str.size();')
assert 'ZipDepth' not in osd and 'Depth Pro 2' not in osd
assert 'm_depth.Loading()' in osd and 'm_depth.Unavailable()' in osd
assert 'coverage(depth.srEvaluations, depth.srDepthRequests)' in osd
assert 'coverage(depth.mfgSubmissions, depth.mfgDepthRequests)' in osd
program = r'''
#include <cassert>
#include <cstdint>
#include <cmath>
#include <iostream>
using std::uint64_t; using std::int64_t;
static uint64_t clockTick = 100;
uint64_t GetTickCount64() { return clockTick; }
struct CDepthGpu { struct Timings {}; };
struct Owner {
''' + counters + methods + r'''
    Counters m_counters;
    uint64_t m_mapRateTick=0, m_mapRateLastPoll=0, m_mapRateBase=0;
    void Poll(uint64_t now) {
''' + rate + r'''
    }
};
int main() {
    Owner o;
    auto& c=o.m_counters;
    o.Poll(100);
    assert(!c.mapRateSampleTick);
    for (unsigned i=1;i<=25;i++) {
        ++c.maps; o.Poll(100+i*40);
    }
    assert(c.mapsPerSecond==25 && c.mapRateSampleTick==1100);
    // A busy or unavailable model contributes zero maps, not an invented FPS.
    for (unsigned i=1;i<=25;i++) o.Poll(1100+i*40);
    assert(c.mapsPerSecond==0 && c.mapRateSampleTick==2100);
    // Pause/resume: no averaging a long idle gap into the next active sample.
    ++c.maps; o.Poll(9000);
    assert(!c.mapRateSampleTick);
    for (unsigned i=1;i<=20;i++) { ++c.maps; o.Poll(9000+i*50); }
    assert(c.mapsPerSecond==20 && c.mapRateSampleTick==10000);
    for (unsigned i=0;i<10000;i++) {
        clockTick++;
        o.RecordSrEvaluation(i%4!=0);
        o.RecordMfgSubmission(i%5==0);
    }
    assert(c.srDepthRequests==10000 && c.srEvaluations==7500);
    assert(c.mfgDepthRequests==10000 && c.mfgSubmissions==2000);
    auto srTick=c.lastSrUseTick, mfgTick=c.lastMfgUseTick;
    clockTick++;
    o.RecordSrEvaluation(false); o.RecordMfgSubmission(false);
    assert(c.lastSrUseTick==srTick && c.lastMfgUseTick==mfgTick);
    assert(c.srDepthRequests==10001 && c.srEvaluations==7500);
    assert(c.mfgDepthRequests==10001 && c.mfgSubmissions==2000);
    std::cout<<"Depth playback statistics PASS (rate, idle gap, zero production, 20002 consumer requests)\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    src = Path(d)/'test.cpp'; exe=Path(d)/'test'
    src.write_text(program)
    subprocess.run([os.environ.get('CXX','clang++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
