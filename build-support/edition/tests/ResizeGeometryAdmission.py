#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# [MFG-B09 | 2026-11-24 17:51 +08:00] Compile actual production admission
# against portable stand-ins. Tests policy, not D3D/NGX/Windows timing.
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'Source/DX11VideoProcessorCompletedFrames.cpp'


def method(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    src = SOURCE.read_text(encoding='utf-8')
    geometry = method(src, 'bool CDX11VideoProcessor::CompletedGeometryChanging(')
    overlap = method(src, 'const char* CDX11VideoProcessor::CompletedOverlapBlock(')
    next_frame = method(src, 'bool CDX11VideoProcessor::NextCompletedFrame(')
    present = method(src, 'HRESULT CDX11VideoProcessor::PresentCompletedVideo(')
    # Compile the real resize scheduling block, not an imitation of its predicate.
    begin = next_frame.index('constexpr ULONGLONG resizeSettleMs = 140;')
    finish = next_frame.index('// Early B-only', begin)
    resize_block = next_frame[begin:finish]
    assert 'if (!scheduled) ApplyPendingWindowResize();' in present
    assert present.count('ApplyPendingWindowResize(') == 1
    assert 'if (m_resizePending && !m_resizeTarget.IsRectEmpty())' in present
    assert 'if (CompletedGeometryChanging())' in method(src, 'REFERENCE_TIME CDX11VideoProcessor::CompletedSubmitLead(')
    code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <vector>
#include "Mfg/BankSelection.h"
using REFERENCE_TIME = int64_t;
using CRefTime = int64_t;
using ULONGLONG = uint64_t;
using UINT = unsigned;
#define FAILED(hr) ((hr) < 0)
namespace mfg { constexpr unsigned MaxGenerated=3; }
struct Size { int cx=1920,cy=1080; };
struct Rect { int l=0,t=0,r=1920,b=1080; bool operator==(const Rect&) const = default;
    int Width() const {return r-l;} int Height() const {return b-t;} };
constexpr int DXGI_SWAP_EFFECT_FLIP_DISCARD=4, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL=3;
struct Desc { UINT Width=1920, Height=1080, Format=87; };
struct Image { Desc desc; };
struct Frame { Image image; Rect picture; bool videoOnly=true; uint64_t fence=1; REFERENCE_TIME due=1000000; };
struct Phase { uint64_t serial=1; unsigned index=0; REFERENCE_TIME presentationTime=1000000; };
struct Batch { Frame target; std::vector<Phase> phases{{}}; };
enum class UpscaleSlot { BeforeMfg };
struct Mfg {
    bool async=true, pending=true;
    unsigned banks=3, reservations=0;
    uint64_t produced=20,sampled=20;
    std::array<mfg::BankOffer,4> offers{{{1,5,0},{2,6,0},{3,7,0},{4,8,0}}};
    mutable unsigned reserveCalls=0;
    bool AsyncProducer() const { return async; }
    unsigned BankCount() const { return banks; }
    bool GenerationPending(uint64_t) const { return pending; }
    std::optional<uint64_t> ReserveNextBankSerial(bool require, std::span<const uint64_t> groups) const {
        ++reserveCalls;
        if (!async) return {};
        auto bank=mfg::SelectReusableBank(std::span(offers.data(),banks),4,produced,sampled,require,groups);
        return bank ? std::optional<uint64_t>(offers[*bank].serial) : std::nullopt;
    }
    unsigned MaxAvailableMultiplier() const { return 2; }
};
static ULONGLONG fakeTick=1000;
static ULONGLONG GetTickCount64() { return fakeTick; }
struct Filter { REFERENCE_TIME stream=0; bool clockOk=true; int StreamTime(CRefTime& out) {out=stream;return clockOk?0:-1;} };
struct CDX11VideoProcessor {
    bool m_completedBudgetKnown=true, m_extendedStartupPending=false,
        m_mfgOriginalPrime=false, before=true, m_resizePending=false;
    unsigned m_mfgMultiplier=2, m_SwapChainFmt=87;
    Size m_processingSize;
    std::deque<Batch> m_completedBatches{Batch{}};
    std::deque<Frame> m_completedFrames{Frame{}};
    Mfg m_mfg;
    uint64_t m_completedByteLimit=1000000, pendingBytes=100;
    ULONGLONG m_resizeLastChangeTick=0, m_completedGeometryTick=0;
    Rect m_resizeTarget, m_windowRect;
    int m_UsedSwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    bool commitFails=false;
    Filter filter; Filter* m_pFilter=&filter;
    unsigned commits=0;
    bool CompletedGeometryChanging() const;
    Rect ProcessingRect() const { return {0,0,m_processingSize.cx,m_processingSize.cy}; }
    bool UpscaleAt(UpscaleSlot) const { return before; }
    uint64_t PendingGeneratedBytes() const { return pendingBytes; }
    REFERENCE_TIME CompletedSubmitLead() { return 15000; }
    void ApplyPendingWindowResize() { ++commits;
        if (commitFails) {m_resizeLastChangeTick=fakeTick; return;}
        m_windowRect=m_resizeTarget; m_resizePending=false; }
    const char* CompletedOverlapBlock(REFERENCE_TIME stream, REFERENCE_TIME margin,
        uint64_t completed, uint64_t committed, uint64_t nextFrameBytes) const;
    void TryCommitResize();
    const char* Admit() const {return CompletedOverlapBlock(0,35000,20,100,100);}
};
''' + geometry + '\n' + overlap + '\nvoid CDX11VideoProcessor::TryCommitResize() {\n' + resize_block + '\n}\n' + r'''
static bool same(const char* a,const char* b) {return (!a&&!b)||(a&&b&&std::strcmp(a,b)==0);}
int main() {
    // Compile the actual predicate: a recent geometry tick must not extend
    // protection once the pending resize has been committed/cancelled.
    CDX11VideoProcessor guard;
    for (auto age : {0ULL, 1ULL, 139ULL, 140ULL, 499ULL, 500ULL, 1000ULL}) {
        guard.m_completedGeometryTick=1000; fakeTick=1000+age;
        guard.m_resizePending=true; assert(guard.CompletedGeometryChanging());
        guard.m_resizePending=false; assert(!guard.CompletedGeometryChanging());
    }
    guard.m_completedGeometryTick=0; assert(!guard.CompletedGeometryChanging());
    std::cout<<"Experimental pending-only geometry predicate PASS\n";
    CDX11VideoProcessor p;
    assert(!p.Admit());
    p.m_resizePending=true;
    assert(!p.Admit()); // formerly unconditionally completed_wait_batch
    p.m_completedBatches.front().target.videoOnly=false;
    assert(same(p.Admit(),"completed_wait_geometry_batch"));
    p.m_completedBatches.front().target.videoOnly=true;
    p.m_completedBatches.front().target.picture.r--;
    assert(same(p.Admit(),"completed_wait_geometry_batch"));
    p.m_completedBatches.front().target.picture.r++;
    p.m_completedBatches.front().target.image.desc.Width--;
    assert(same(p.Admit(),"completed_wait_geometry_batch"));
    p.m_completedBatches.front().target.image.desc.Width++;
    p.m_completedBatches.front().target.image.desc.Format++;
    assert(same(p.Admit(),"completed_wait_geometry_batch"));
    p.m_completedBatches.front().target.image.desc.Format--;
    p.m_processingSize.cx=0;
    assert(same(p.Admit(),"completed_wait_geometry_batch"));
    p.m_processingSize.cx=1920;
    p.m_completedBatches.push_back(Batch{});
    p.m_completedBatches.back().target.videoOnly=false;
    assert(same(p.Admit(),"completed_wait_geometry_batch"));
    p.m_completedBatches.pop_back();
    // Geometry-independent route must match ALL existing normal admission
    // decisions, including rejected ones, across varied real bank offers.
    std::mt19937 rng(20261124);
    unsigned accepted=0,rejected=0;
    for (int i=0;i<100000;++i) {
        CDX11VideoProcessor x;
        x.m_completedBudgetKnown=rng()%10!=0;
        x.m_extendedStartupPending=rng()%10==0;
        x.m_mfgOriginalPrime=rng()%10==0;
        x.m_mfgMultiplier=(rng()%10==0)?0:2;
        x.before=rng()%2;
        x.m_mfg.async=rng()%5!=0;
        x.m_mfg.pending=rng()%2;
        x.m_mfg.banks=2+rng()%3;
        x.m_mfg.produced=rng()%12;
        x.m_mfg.sampled=rng()%12;
        for (unsigned j=0;j<x.m_mfg.banks;++j) {
            x.m_mfg.offers[j].producerFence=rng()%12;
            x.m_mfg.offers[j].presenterFence=rng()%12;
        }
        if (rng()%4==0) { x.m_completedBatches.push_back(Batch{}); x.m_completedBatches.back().phases[0].serial=2; }
        if (rng()%10==0) x.m_completedBatches.front().phases.clear();
        else if (rng()%5==0) x.m_completedBatches.front().phases[0].index=mfg::MaxGenerated;
        if (rng()%5==0) x.m_completedFrames.clear();
        else { x.m_completedFrames.front().due=rng()%100000; x.m_completedFrames.front().fence=rng()%30; }
        x.m_completedByteLimit=(rng()%2)?1000000:200;
        const auto a=x.Admit(); const auto calls=x.m_mfg.reserveCalls;
        x.m_resizePending=true;
        const auto b=x.Admit();
        assert(same(a,b)); assert(x.m_mfg.reserveCalls==2*calls);
        if (a) ++rejected; else ++accepted;
    }
    assert(accepted>0 && rejected>0);
    // High-density resize timeline: resize stays pending throughout the interval.
    // The original policy would refuse every overlapping opportunity in the
    // interval; the new code has the identical decisions as normal geometry.
    CDX11VideoProcessor stress;
    unsigned opportunities=0;
    for (unsigned ms=0;ms<8000;ms+=16) {
        stress.m_resizePending=true;
        assert(!stress.Admit()); ++opportunities;
    }
    // Commit only after last event settles, never just ahead of a due head;
    // empty queues and unavailable clock retain a path to commit.
    CDX11VideoProcessor r;
    r.m_resizePending=true; r.m_resizeLastChangeTick=900; fakeTick=1039;
    r.TryCommitResize(); assert(r.commits==0);
    fakeTick=1040; r.m_completedFrames.front().due=35000;
    r.TryCommitResize(); assert(r.commits==0 && r.m_resizePending);
    r.m_completedFrames.front().due=35001;
    r.TryCommitResize(); assert(r.commits==1 && !r.m_resizePending);
    assert(!r.CompletedGeometryChanging());
    r.m_resizePending=true; r.m_completedFrames.clear();
    r.TryCommitResize(); assert(r.commits==2);
    r.m_resizePending=true; r.m_completedFrames.push_back(Frame{}); r.filter.clockOk=false;
    r.TryCommitResize(); assert(r.commits==3);
    // B12: a settled flip expansion cannot starve behind a perpetually due
    // head. No override before 190 ms, no resize per WM_SIZE, no repeated
    // commit after success, same retry backoff as ApplyPendingWindowResize.
    for (int flip : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL}) {
        CDX11VideoProcessor e;
        e.m_UsedSwapEffect=flip; e.m_resizePending=true;
        e.m_resizeTarget={0,0,3840,2160}; e.m_completedFrames.front().due=0;
        e.m_resizeLastChangeTick=1000;
        for (fakeTick=1000;fakeTick<1190;++fakeTick) {e.TryCommitResize();assert(e.commits==0);}
        e.TryCommitResize();assert(e.commits==1 && !e.m_resizePending);
        e.TryCommitResize();assert(e.commits==1);
        // After a new event, the deadline moves with the most recent change.
        e.m_resizeTarget={0,0,4096,2160}; e.m_resizePending=true;
        e.m_resizeLastChangeTick=1200; fakeTick=1389;
        e.TryCommitResize();assert(e.commits==1);
        fakeTick=1390; e.commitFails=true; e.TryCommitResize();
        assert(e.commits==2 && e.m_resizePending && e.m_resizeLastChangeTick==1390);
        assert(e.CompletedGeometryChanging());
        fakeTick=1579;e.TryCommitResize();assert(e.commits==2);
        fakeTick=1580;e.commitFails=false;e.TryCommitResize();assert(e.commits==3 && !e.m_resizePending);
        assert(!e.CompletedGeometryChanging());
    }
    CDX11VideoProcessor shrink;
    shrink.m_resizePending=true;shrink.m_resizeTarget={0,0,1280,720};
    shrink.m_completedFrames.front().due=0;fakeTick=10000;
    shrink.TryCommitResize();assert(shrink.commits==0); // no changed shrink policy
    shrink.m_resizeTarget={0,0,3840,2160};shrink.m_UsedSwapEffect=0;
    shrink.TryCommitResize();assert(shrink.commits==0); // no changed blt policy
    shrink.m_UsedSwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    shrink.m_resizeTarget={0,0,1920,1200};shrink.TryCommitResize();assert(shrink.commits==1); // height-only growth
    CDX11VideoProcessor early;
    early.m_resizePending=true;early.m_resizeTarget={0,0,3840,2160};
    early.m_resizeLastChangeTick=1000;fakeTick=1140;
    early.m_completedFrames.front().due=35001;
    early.TryCommitResize();assert(early.commits==1); // prefer earlier safe gap
    std::cout<<"B12 settled expansion deadline, retry, shrink/blt preservation PASS\n";
    std::cout<<"Actual overlap: "<<accepted<<" admits, "<<rejected<<" rejects; identical during fixed-canvas resize\n";
    std::cout<<"Fixed-canvas stress opportunities: "<<opportunities<<"; geometry-dependent/mixed/format guards and deadline-aware commit PASS\n";
}
'''
    with tempfile.TemporaryDirectory() as folder:
        cpp = Path(folder) / 'test.cpp'
        exe = Path(folder) / 'test'
        cpp.write_text(code, encoding='utf-8')
        flags = ['-std=c++20', '-O2', '-Wall', '-Wextra', '-I', str(ROOT/'Source')]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
        subprocess.run([args.cxx, *flags, str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
