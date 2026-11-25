#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [MFG-B10 | 2026-11-24 23:56 +08:00] Extract actual DXGI classification.
from pathlib import Path
import argparse, os, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[3]

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--cxx',default=os.environ.get('CXX','g++')); args=parser.parse_args()
    s=(ROOT/'Source/Edition/DX11Statistics.cpp').read_text()
    begin=s.index('const auto previousCount = m_mfgDxgiLastPresentCount;')
    end=s.index('m_mfgRateTick=rateTick;',begin)
    block=s[begin:end]
    code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <random>
using HRESULT=int32_t;
constexpr HRESULT DXGI_ERROR_FRAME_STATISTICS_DISJOINT=-1, E_FAIL=-2, S_OK=0;
#define SUCCEEDED(x) ((x)>=0)
struct DXGI_FRAME_STATISTICS {unsigned PresentCount=0,PresentRefreshCount=0,SyncRefreshCount=0;struct {int64_t QuadPart=0;} SyncQPCTime{};};
namespace mfg { struct Trace {const char* status=nullptr;long hr=0;bool Enabled()const{return true;}template<class... Args>void Record(const char* name,int64_t,int64_t,int64_t,uint64_t,unsigned,unsigned,unsigned,int64_t,bool,uint64_t,long h,Args...){status=name;hr=h;}}; Trace& SourceCadenceTrace(){static Trace trace;return trace;} }
struct State {
    bool m_mfgDxgiHasBaseline=false,m_mfgDxgiAvailable=false;
    unsigned m_mfgDxgiLastPresentCount=0;
    double m_mfgDxgiDisplayFps=0.0,m_dRefreshPeriodMs=1000.0/60;
    const wchar_t* m_mfgDxgiStatus=L"baseline";
    uint64_t m_mfgLostGenerated=0;
    int64_t m_mfgRateTick=0;
    void Observe(bool supported,HRESULT statsHr,DXGI_FRAME_STATISTICS dxgi,double seconds) {
        const int64_t rateTick=1000;
''' + block + r'''
    }
};
int main(){
    State s;
    s.Observe(true,S_OK,{100},1); assert(!s.m_mfgDxgiAvailable && !std::wcscmp(s.m_mfgDxgiStatus,L"baseline"));
    s.Observe(true,S_OK,{160},1); assert(s.m_mfgDxgiAvailable && s.m_mfgDxgiDisplayFps==60);
    s.Observe(true,DXGI_ERROR_FRAME_STATISTICS_DISJOINT,{},1); assert(!s.m_mfgDxgiAvailable && !s.m_mfgDxgiHasBaseline && !std::wcscmp(s.m_mfgDxgiStatus,L"disjoint"));
    s.Observe(true,S_OK,{220},1); assert(!s.m_mfgDxgiAvailable);
    s.Observe(true,S_OK,{280},1); assert(s.m_mfgDxgiAvailable);
    s.Observe(true,S_OK,{200},1); assert(!s.m_mfgDxgiAvailable && !std::wcscmp(s.m_mfgDxgiStatus,L"count reversed"));
    s.Observe(true,S_OK,{300},1); assert(!s.m_mfgDxgiAvailable && !std::wcscmp(s.m_mfgDxgiStatus,L"rate rejected"));
    s.Observe(true,E_FAIL,{},1); assert(!std::wcscmp(s.m_mfgDxgiStatus,L"API failed"));
    s.Observe(false,E_FAIL,{},1); assert(!std::wcscmp(s.m_mfgDxgiStatus,L"unsupported"));
    s.m_dRefreshPeriodMs=0; s.Observe(true,S_OK,{400},1); s.Observe(true,S_OK,{460},1); assert(!std::wcscmp(s.m_mfgDxgiStatus,L"refresh unknown"));
    // Compare instrumented state against the original B09 numerical rule.
    std::mt19937 rng(20261124);
    for(int i=0;i<100000;++i) {
        const bool supported=rng()%10!=0; const HRESULT hr=supported && rng()%5!=0?S_OK:E_FAIL;
        const unsigned count=rng()%5000; const double seconds=1.0+(rng()%200)*.01;
        s.m_dRefreshPeriodMs=rng()%10?1000.0/(30+rng()%211):0;
        const bool valid=hr>=0 && s.m_mfgDxgiHasBaseline && count>=s.m_mfgDxgiLastPresentCount && s.m_dRefreshPeriodMs>0 &&
            double(count-s.m_mfgDxgiLastPresentCount)/seconds<=1000.0/s.m_dRefreshPeriodMs*1.2+1.0;
        const double expected=valid?double(count-s.m_mfgDxgiLastPresentCount)/seconds:s.m_mfgDxgiDisplayFps;
        const auto expectedCount=hr>=0?count:s.m_mfgDxgiLastPresentCount;
        s.Observe(supported,hr,{count},seconds);
        assert(s.m_mfgDxgiAvailable==valid && s.m_mfgDxgiHasBaseline==(hr>=0) && s.m_mfgDxgiDisplayFps==expected && s.m_mfgDxgiLastPresentCount==expectedCount);
        assert(mfg::SourceCadenceTrace().hr==hr);
    }
    std::cout<<"PASS: DXGI status branches and 100000 B09-rate equivalence cases\n";
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(code)
        subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-O2',str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
