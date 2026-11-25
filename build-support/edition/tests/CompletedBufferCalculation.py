#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [MFG-B10 | 2026-11-24 23:56 +08:00] Test the extracted production methods.
from pathlib import Path
import argparse
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parents[3]

def extract(s, name):
    start = s.index(name)
    pos = s.index('{', start)
    level = 1
    end = pos + 1
    while level:
        level += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    method = extract((ROOT/'Source/DX11VideoProcessorCompletedFrames.cpp').read_text(),
                     'CDX11VideoProcessor::CompletedBufferTime CDX11VideoProcessor::GetCompletedBufferTime(')
    percent = extract((ROOT/'Source/Edition/StatisticsFormat.h').read_text(), 'inline unsigned BufferPercent(')
    code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <random>
#include "Edition/CompletedBufferAudit.h"
using REFERENCE_TIME = int64_t;
struct Frame { int64_t due; uint64_t fence; };
struct Fence { uint64_t value=UINT64_MAX-1; uint64_t GetCompletedValue() const {return value;} };
struct CDX11VideoProcessor {
    struct CompletedBufferTime {
        REFERENCE_TIME ready=0,queued=0;
        size_t readyFrames=0,queuedFrames=0;
        uint64_t fence=UINT64_MAX;
        unsigned nonIncreasing=0;
    };
    std::deque<Frame> m_completedFrames;
    Fence fence;
    Fence* m_completedFence=&fence;
    CompletedBufferTime GetCompletedBufferTime(REFERENCE_TIME stream) const;
};
''' + method + '\nnamespace edition {\n' + percent + '\n}\n' + r'''
using Cutoff=edition::CompletedBufferAudit::Cutoff;
int main() {
    constexpr int64_t target=20000000, dt=166667;
    using edition::BufferPercent;
    assert(BufferPercent(199999,target)==0 && BufferPercent(200000,target)==1);
    assert(BufferPercent(target,target)==100 && BufferPercent(target-1,target)==99);
    assert(BufferPercent(-1,target)==0 && BufferPercent(10,0)==0);
    CDX11VideoProcessor p;
    // At time zero the next 120 frames are already complete. Insert one
    // duplicate due at the boundary, then keep the producer running. This
    // reproduces the reported shape without modeling a GPU stall.
    for(int i=1;i<=120;++i) p.m_completedFrames.push_back({i*dt,uint64_t(i)});
    p.m_completedFrames.push_back({120*dt,121});
    uint64_t serial=121;
    int64_t tail=120*dt;
    unsigned previous=100;
    std::cout << "time_ms,percent,ready_frames,queued_frames,signaled_frames,queued_ms,cutoff\n";
    for(int i=0;i<=121;++i) {
        const auto stream=i*dt;
        while(!p.m_completedFrames.empty() && p.m_completedFrames.front().due<stream) {
            p.m_completedFrames.pop_front();
            tail+=dt;
            p.m_completedFrames.push_back({tail,++serial});
        }
        const auto r=p.GetCompletedBufferTime(stream);
        const auto a=edition::AuditCompletedBuffer(p.m_completedFrames,r.fence,r.readyFrames);
        const auto pct=BufferPercent(r.ready,target);
        assert(r.queuedFrames==121 && a.signaledFrames==121);
        assert(r.queued>=target-dt);
        if(i<121) {
            assert(a.cutoff==Cutoff::Order && a.readyEnd==120*dt && r.nonIncreasing==1);
            assert(pct<=previous);
        } else assert(a.cutoff==Cutoff::None && pct>=99);
        if(i==120) assert(pct==0 && r.readyFrames==1 && a.signaledFrames==121);
        if(i==0 || i%30==0 || i==121)
            std::cout << stream/10000 << ',' << pct << ',' << r.readyFrames << ',' << r.queuedFrames << ','
                      << a.signaledFrames << ',' << r.queued/10000 << ',' << (a.cutoff==Cutoff::Order?"order":"none") << '\n';
        previous=pct;
    }
    // Ordered fence timeline: later values cannot be signaled while an earlier
    // greater value is pending. A healthy monotonic-fence queue has no hidden
    // signaled suffix. Test order, fence, and invalid fence independently.
    p.m_completedFrames={{dt,1},{2*dt,2},{3*dt,3},{4*dt,4}};
    p.fence.value=2;
    auto r=p.GetCompletedBufferTime(0);
    auto a=edition::AuditCompletedBuffer(p.m_completedFrames,r.fence,r.readyFrames);
    assert(a.cutoff==Cutoff::Fence && a.cutoffIndex==2 && a.signaledFrames==r.readyFrames && a.signaledEnd==a.readyEnd);
    p.m_completedFrames[2].due=dt;
    r=p.GetCompletedBufferTime(0); a=edition::AuditCompletedBuffer(p.m_completedFrames,r.fence,r.readyFrames);
    assert(a.cutoff==Cutoff::OrderAndFence && a.cutoffDue==dt && a.priorDue==2*dt);
    p.fence.value=UINT64_MAX;
    r=p.GetCompletedBufferTime(0); a=edition::AuditCompletedBuffer(p.m_completedFrames,r.fence,r.readyFrames);
    assert(a.cutoff==Cutoff::NoFence && r.ready==0);
    p.m_completedFence=nullptr;
    r=p.GetCompletedBufferTime(0); assert(r.fence==UINT64_MAX && r.ready==0);
    p.m_completedFence=&p.fence;
    // Independent index scan verifies the actual production prefix against
    // the diagnostic classification on varying timestamps and fence states.
    std::mt19937 rng(20261124);
    for(unsigned j=0;j<100000;++j) {
        p.m_completedFrames.clear(); p.fence.value=rng()%20;
        int64_t due=10000000;
        const int n=rng()%32;
        const int64_t stream=10000000+int(rng()%1000000)-500000;
        for(int i=0;i<n;++i) { due+=int(rng()%7)-2; p.m_completedFrames.push_back({due,uint64_t(rng()%20)}); }
        r=p.GetCompletedBufferTime(stream); a=edition::AuditCompletedBuffer(p.m_completedFrames,r.fence,r.readyFrames);
        size_t prefix=0,signaled=0; int64_t mx=-1, q=-1;
        for(size_t i=0;i<p.m_completedFrames.size();++i) {
            const auto& f=p.m_completedFrames[i]; q=std::max(q,f.due);
            if(f.fence<=p.fence.value) {++signaled;mx=std::max(mx,f.due);}
            if(prefix==i && f.fence<=p.fence.value && (!i || f.due>p.m_completedFrames[i-1].due)) ++prefix;
        }
        assert(prefix==r.readyFrames && signaled==a.signaledFrames && mx==a.signaledEnd && q==a.queuedEnd);
        assert(r.queued==std::max(int64_t(0),q-stream));
        if(prefix==size_t(n)) assert(a.cutoff==Cutoff::None);
        else assert(a.cutoff!=Cutoff::None && a.cutoffIndex==prefix);
        assert(r.ready==(prefix?std::max(int64_t(0),p.m_completedFrames[prefix-1].due-stream):0));
    }
    std::cout << "PASS: 100000 independent audit comparisons, exact 0% counterexample, fence/percent boundaries\n";
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        src=Path(tmp)/'test.cpp'; exe=Path(tmp)/'test'
        src.write_text(code)
        cmd=[args.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-O2','-I',str(ROOT/'Source'),str(src),'-o',str(exe)]
        if args.sanitize: cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
        subprocess.run(cmd,check=True)
        subprocess.run([str(exe)],check=True)
if __name__=='__main__': main()
