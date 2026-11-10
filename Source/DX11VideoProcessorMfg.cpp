// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Helper.h"
#include "DX11VideoProcessor.h"
// No NR/SR/Plus enhancement shared header is needed.
#include "VideoRenderer.h"
#include "DLSS/NgxRuntime.h"
#include "Mfg/DisplayCadence.h"
#include "Times.h"
#include "Mfg/CadenceTrace.h"
#include <limits>
#include <cwchar>
extern bool g_bPresent;
namespace {
// First integrated Live Flow DLSSG group/pacing path. Set to 0 for v7.
// This is a new group scheduler around the existing NGX producer, not a
// claim that all Manager GPU/presenter internals have already been ported.
// Stage 3.2 experiments are OFF unless explicitly enabled before MPC-BE
// starts. The user-validated CFR Golden remains the default code path.
bool DynamicSourceTimingEnabled() {
    static const bool enabled=[] {
        wchar_t value[8]{};
        const DWORD len=GetEnvironmentVariableW(L"MPCVR_MFG_DYNAMIC_TIMING",value,_countof(value));
        return len>0 && len<_countof(value) && value[0]==L'1';
    }();
    return enabled;
}
bool LiveFlowCoreEnabled() {
    static const bool enabled=[] {
        wchar_t value[8]{};
        const DWORD len=GetEnvironmentVariableW(L"MPCVR_MFG_LIVEFLOW_CORE",value,_countof(value));
        return len==0 || value[0]!=L'0';
    }();
    return enabled;
}
// Restore unmodified v5 behavior for a direct A/B run:
//   set MPCVR_MFG_ADAPTIVE_AHEAD=0
// A/B switch: set MPCVR_MFG_DISPLAY_PACING=0 before launching MPC-BE to
// restore v6's exact-deadline presentation behavior.
bool DisplayPacingEnabled() {
    static const bool enabled=[] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"MPCVR_MFG_DISPLAY_PACING",value,_countof(value))==0 || value[0]!=L'0';
    }();
    return enabled;
}
bool DisplayPreparationEnabled() {
    static const bool enabled=[] {
        wchar_t option[8]{};
        return GetEnvironmentVariableW(L"MPCVR_MFG_PRECOMPOSE",option,_countof(option))==0 || option[0]!=L'0';
    }();
    return enabled;
}
bool AdaptiveAheadEnabled() {
    static const bool enabled = [] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"MPCVR_MFG_ADAPTIVE_AHEAD",value,_countof(value)) == 0 || value[0] != L'0';
    }();
    return enabled;
}
}

bool CDX11VideoProcessor::MfgPostVsrRequested() const {
    if (m_mfgProcessingMode != 1 || m_srcRectHeight <= 0) return false;
    // NGX can internally scale to MPCVR_MFG_MAX_HEIGHT. The dedicated
    // post-VSR VP requires real and generated frames to share source size.
    // Keep the Golden path if a user-selected MFG cap is smaller.
    wchar_t text[16]{};
    const DWORD n=GetEnvironmentVariableW(L"MPCVR_MFG_MAX_HEIGHT",text,_countof(text));
    UINT cap=1440;
    if(n>0 && n<_countof(text)) {
        wchar_t* end=nullptr;
        const unsigned long value=std::wcstoul(text,&end,10);
        if(end!=text && *end==L'\0') {
            if(value==0) cap=std::numeric_limits<UINT>::max();
            else if(value>=360 && value<=8192) cap=UINT(value);
        }
    }
    return UINT(m_srcRectHeight)<=cap;
}

void CDX11VideoProcessor::ResetMfgPostVsr() {
    m_mfgPostVsrBGRA.Release();
    m_mfgPreVsrVP.ReleaseVideoDevice();
    m_mfgPostVsrVP.ReleaseVideoDevice();
    m_mfgPostVsrReady = false;
    m_mfgPostVsrWidth = m_mfgPostVsrHeight = 0;
    m_mfgPostVsrSourceFmt = DXGI_FORMAT_UNKNOWN;
    m_mfgPostVsrSetting = SUPERRES_Disable;
}

bool CDX11VideoProcessor::EnsureMfgPostVsr() {
    if (m_mfgProcessingMode != 1 || m_mfgPostVsrBroken || !m_D3D11VP.IsReady() ||
        !m_bVPUseSuperRes || m_VendorId != PCIV_NVIDIA ||
        m_srcDXGIFormat == DXGI_FORMAT_UNKNOWN || !m_srcWidth || !m_srcHeight ||
        !m_srcRectWidth || !m_srcRectHeight || m_srcParams.CDepth != 8) return false;
    if (m_mfgPostVsrReady && m_mfgPostVsrWidth == UINT(m_srcRectWidth) &&
        m_mfgPostVsrHeight == UINT(m_srcRectHeight) &&
        m_mfgPostVsrSourceFmt == m_srcDXGIFormat &&
        m_mfgPostVsrSetting == m_iVPSuperRes) return true;
    ResetMfgPostVsr();
    const auto fail=[this](const wchar_t* stage,HRESULT hr) {
        DLog(L"Hybrid post-MFG RTX VSR not available at {}: {} (keeping Golden path)",stage,HR2Str(hr));
        ResetMfgPostVsr();
        m_mfgPostVsrBroken = true;
        return false;
    };
    HRESULT hr=m_mfgPreVsrVP.InitVideoDevice(m_pDevice,m_pDeviceContext,m_VendorId);
    if (FAILED(hr)) return fail(L"pre-VP device",hr);
    DXGI_FORMAT preOutput=DXGI_FORMAT_B8G8R8A8_UNORM;
    hr=m_mfgPreVsrVP.InitVideoProcessor(m_srcDXGIFormat,m_srcWidth,m_srcHeight,
        m_srcExFmt,DEINT_Disable,false,preOutput);
    if (FAILED(hr) || preOutput!=DXGI_FORMAT_B8G8R8A8_UNORM)
        return fail(L"pre-VP YUV to BGRA",FAILED(hr)?hr:E_INVALIDARG);
    // A different VP owns the NVIDIA extension. No per-frame VSR toggle on
    // the decoder's existing VP: changing that VP risks Golden playback.
    hr=m_mfgPostVsrVP.InitVideoDevice(m_pDevice,m_pDeviceContext,m_VendorId);
    if (FAILED(hr)) return fail(L"post-VP device",hr);
    DXVA2_ExtendedFormat rgb{}; // full-range SDR RGB, not the decoder's YUV metadata
    DXGI_FORMAT postOutput=DXGI_FORMAT_B8G8R8A8_UNORM;
    hr=m_mfgPostVsrVP.InitVideoProcessor(DXGI_FORMAT_B8G8R8A8_UNORM,
        UINT(m_srcRectWidth),UINT(m_srcRectHeight),rgb,DEINT_Disable,false,postOutput);
    if (FAILED(hr) || postOutput!=DXGI_FORMAT_B8G8R8A8_UNORM)
        return fail(L"post-VP RGB input",FAILED(hr)?hr:E_INVALIDARG);
    hr=m_mfgPostVsrVP.SetSuperRes(m_iVPSuperRes);
    if (hr!=S_OK) return fail(L"post-VP NVIDIA VSR extension",hr);
    m_mfgPostVsrWidth=UINT(m_srcRectWidth);
    m_mfgPostVsrHeight=UINT(m_srcRectHeight);
    m_mfgPostVsrSourceFmt=m_srcDXGIFormat;
    m_mfgPostVsrSetting=m_iVPSuperRes;
    m_mfgPostVsrReady=true;
    DLog(L"Hybrid MFG -> RTX VSR: D3D11 driver accepted RGB VP extension at {}x{}; visual effect NOT yet verified",
        m_mfgPostVsrWidth,m_mfgPostVsrHeight);
    return true;
}

void CDX11VideoProcessor::StopMfg(bool release) {
    // Presenter never blocks on RendererLock, so joining under it is safe.
    m_mfgThread.Stop();
    m_mfgPreparedFrame.reset();
    m_mfgPreparedOriginalPending.store(0,std::memory_order_relaxed);
    m_mfgPreparedTexture.Release();
    m_mfgPreparedDesc = {};
    m_mfgPreparationBroken = false;
    m_mfgPrepareLeadUs.store(0,std::memory_order_relaxed);
    m_mfgPrepareLastDrawUs.store(0,std::memory_order_relaxed);
    m_mfgPrepareReadyMarginUs.store(0,std::memory_order_relaxed);
    m_mfgDisplayPacing.Reset(); // safe: presenter thread has joined
    m_mfgDisplayLeadUs.store(0,std::memory_order_relaxed);
    m_mfgDisplayLastLateUs.store(0,std::memory_order_relaxed);
    m_mfgDisplayP95NeedUs.store(0,std::memory_order_relaxed);
    m_mfgDisplaySamples.store(0,std::memory_order_relaxed);
    {std::lock_guard guard(m_mfgQueueLock);m_mfgQueue.Clear();m_mfgLiveFlowGroups.Clear();}
	{
		std::lock_guard paceGuard(m_mfgPacingLock);
		m_mfgPacing.Reset();
		m_mfgGroupPacing.Reset();
		m_mfgExtraAheadTicks.store(0,std::memory_order_relaxed);
		m_mfgReadyLeadUs.store(0,std::memory_order_relaxed);
	}
    // The swap chain and/or current playback timeline may have changed.
    // Avoid mixing rates from different modes after seeking or resizing.
    {
        std::lock_guard rateGuard(m_mfgRateLock);
        m_mfgRateTick=0;
        m_mfgDxgiHasBaseline=false;
        m_mfgDxgiAvailable=false;
    }
    m_mfgEffectiveMultiplier.store(0, std::memory_order_relaxed);
    m_mfgBudgetCandidate=0;
    m_mfgBudgetStreak=0;
    if(release) {
        m_mfg.Reset();m_mfgPresentFailed=false;m_mfgInputModeKnown=false;
        m_mfgLastPostVsrInput=false;
        m_mfgAutoEpoch=m_mfgAutoLastPts=-1;
        ResetMfgPostVsr();m_mfgPostVsrBroken=false;
    }
}
bool CDX11VideoProcessor::NextMfg(int64_t& deadline) {
    const double hz=m_dRefreshPeriodMs>0.0?1000.0/m_dRefreshPeriodMs:0.0;
    const double leadMs=DisplayPacingEnabled()?m_mfgDisplayPacing.Start(hz):0.0;
    m_mfgDisplayLeadUs.store(int64_t(leadMs*1000.0),std::memory_order_relaxed);
    const auto leadTicks=int64_t(leadMs*GetPreciseTicksPerSecond()/1000.0);
    if(m_mfgPreparedFrame) {
        deadline = mfg::DisplayPreparation::Wake(m_mfgPreparedFrame->deadline,leadTicks,true,0);
        return true;
    }
    const bool prepare=LiveFlowCoreEnabled() && DisplayPreparationEnabled() && !m_mfgPreparationBroken;
    const double prepareMs=prepare ? mfg::DisplayPreparation::LeadMs(hz) : 0.0;
    m_mfgPrepareLeadUs.store(int64_t(prepareMs*1000.0),std::memory_order_relaxed);
    const auto prepareTicks=int64_t(prepareMs*GetPreciseTicksPerSecond()/1000.0);
    std::lock_guard guard(m_mfgQueueLock);
    return LiveFlowCoreEnabled() ? m_mfgLiveFlowGroups.Next(deadline,std::max(leadTicks,prepareTicks))
                                 : m_mfgQueue.Next(deadline,leadTicks);
}
bool CDX11VideoProcessor::QueueMfg(ID3D11Texture2D* image,REFERENCE_TIME timestamp,bool nativeInput,bool postVsrInput) {
    if(m_mfgThread.Failed())m_mfgPresentFailed=true;
    if(m_mfgPresentFailed) {StopMfg(false);return false;}
    if(m_mfg.Failed())return false;
    const unsigned requested=m_mfgMultiplier.load(std::memory_order_relaxed);
    const double refreshHz=m_dRefreshPeriodMs>0.0 ? 1000.0/m_dRefreshPeriodMs : 0.0;
    const double sourceFps=m_pFilter->m_FrameStats.GetAverageFps();
    const bool autoMode=requested==mfg::AutoMultiplier;
    if (autoMode) {
        // Do not treat a speed change or seek as one enormous interpolation interval.
        if(m_mfgAutoLastPts<0 || timestamp<=m_mfgAutoLastPts ||
           timestamp-m_mfgAutoLastPts>2500000) {
            m_mfgAutoEpoch=timestamp;
            m_mfgAutoLastPts=-1;
        }
    }
    const unsigned proposed=autoMode
        ? std::min(mfg::AutoCadenceMultiplier(m_mfgAutoLastPts,timestamp,m_mfgAutoEpoch,refreshHz),
                   m_mfg.MaxAvailableMultiplier())
        : mfg::DisplayBudgetMultiplier(requested,sourceFps,refreshHz);
    if(autoMode) m_mfgAutoLastPts=timestamp;
    unsigned effective=m_mfgEffectiveMultiplier.load(std::memory_order_relaxed);
    if (autoMode) {
        // An x2/x3/x2 cadence is intentional: do not debounce it for 8 frames.
        effective=proposed;
        m_mfgEffectiveMultiplier.store(effective,std::memory_order_relaxed);
    } else if (proposed != effective) {
        if (proposed != m_mfgBudgetCandidate) {m_mfgBudgetCandidate=proposed;m_mfgBudgetStreak=1;}
        else if (m_mfgBudgetStreak<8) ++m_mfgBudgetStreak;
        // Avoid repeated NGX reconfiguration when frame-time estimates hover
        // near a multiplier threshold. Initial selection is immediate.
        if (effective==0 || m_mfgBudgetStreak>=8) {
            effective=proposed;
            m_mfgEffectiveMultiplier.store(effective,std::memory_order_relaxed);
            m_mfgBudgetStreak=0;
        }
    } else {m_mfgBudgetCandidate=effective;m_mfgBudgetStreak=0;}
    // Auto x1 needs no NGX work, but MUST remain in the same queue. A direct
    // renderer fallback would clear pending originals or invert PTS order.
    // A same-sized texture can change meaning when a display mode toggles.
    // Do not interpolate across native-video and full-backbuffer layouts.
    if (m_mfgInputModeKnown && (m_mfgLastNativeInput != nativeInput ||
        m_mfgLastPostVsrInput != postVsrInput)) {
        StopMfg(false);
        m_mfg.ResetHistory();
    }
    m_mfgInputModeKnown=true;
    m_mfgLastNativeInput=nativeInput;
    m_mfgLastPostVsrInput=postVsrInput;
    mfg::Batch batch;
    const auto generateStart=int64_t(GetPreciseTick());
    const bool generated=effective<2
        ? m_mfg.CaptureOriginal(image,timestamp,batch)
        : m_mfg.Generate(m_pDevice,m_pDeviceContext,image,effective,timestamp,NgxRuntime::ModuleDir()+L"\\dlss",batch);
    const auto generateEnd=int64_t(GetPreciseTick());
    if (generateEnd>generateStart) m_mfgGenerateTicks.fetch_add(uint64_t(generateEnd-generateStart),std::memory_order_relaxed);
    m_mfgGenerateCount.fetch_add(1,std::memory_order_relaxed);
    if(!generated) {
        StopMfg(false);DLog(L"MFG unavailable: {}",m_mfg.Status());return false;
    }
    CRefTime stream;if(FAILED(m_pFilter->StreamTime(stream))) {StopMfg();return false;}
    wchar_t trace[2]{};
    if(GetEnvironmentVariableW(L"MPCVR_MFG_TRACE",trace,2))
        fprintf(stderr,"MFG_READY stamp=%lld stream=%lld lead_us=%lld generated=%u ahead=%d path=%s\n",timestamp,REFERENCE_TIME(stream),(timestamp-REFERENCE_TIME(stream))/10,batch.generated,GetRenderAhead(),nativeInput ? "source-first" : "legacy");
    const auto qpc=int64_t(GetPreciseTick());
    // Manager-inspired work-ahead control, grounded in actual DirectShow PTS:
    // if a generated frame becomes ready too close to its deadline, ask the
    // upstream renderer for a little more read-ahead on *future* samples.
    // Never move frame deadlines or prioritize a generated frame over a real one.
    if (AdaptiveAheadEnabled() && batch.generated>0 && batch.previous>=0 &&
        batch.current>batch.previous && batch.current-batch.previous<=2500000) {
        const double spanMs=double(batch.current-batch.previous)/10000.0;
        const double leadMs=(double(batch.previous) +
            double(batch.current-batch.previous)/double(batch.generated+1) +
            double(m_rtVideoDelay) - double(REFERENCE_TIME(stream)))/10000.0;
        if (std::isfinite(leadMs)) {
            m_mfgReadyLeadUs.store(int64_t(std::clamp(leadMs,-1000.0,1000.0)*1000.0),std::memory_order_relaxed);
            if (leadMs < 0) m_mfgReadyLate.fetch_add(1,std::memory_order_relaxed);
            // At most half an input interval and 12ms; limit bank pressure.
            const double limitMs=(std::min)(12.0,spanMs*0.5);
            double extraMs=0.0;
            {
                std::lock_guard paceGuard(m_mfgPacingLock);
                extraMs=LiveFlowCoreEnabled() ? m_mfgGroupPacing.Observe(leadMs,limitMs)
                                              : m_mfgPacing.Observe(leadMs,limitMs);
            }
            m_mfgExtraAheadTicks.store(int64_t(extraMs*10000.0),std::memory_order_relaxed);
        }
    }
    {std::lock_guard guard(m_mfgQueueLock);
        const auto lost=LiveFlowCoreEnabled()
            ? m_mfgLiveFlowGroups.Add(batch,qpc,stream,GetPreciseTicksPerSecond(),m_rtVideoDelay,
                m_dRefreshPeriodMs>0?1000./m_dRefreshPeriodMs:0.,nativeInput,m_mfg.BankCount(),autoMode,
                DynamicSourceTimingEnabled())
            : m_mfgQueue.Add(batch,qpc,stream,GetPreciseTicksPerSecond(),m_rtVideoDelay,
                m_dRefreshPeriodMs>0?1000./m_dRefreshPeriodMs:0.,nativeInput,m_mfg.BankCount(),autoMode);
        if(lost.Total()) {
            m_mfgSkipped.fetch_add(lost.Total(),std::memory_order_relaxed);
            m_mfgLostReal.fetch_add(lost.real,std::memory_order_relaxed);
            m_mfgLostGenerated.fetch_add(lost.generated,std::memory_order_relaxed);
            m_mfgDroppedOnAdd.fetch_add(lost.Total(),std::memory_order_relaxed);
        }
    }
    // Observation-only: source arrival at this point is AFTER NGX submit,
    // not raw decoder arrival. Never use these rows to alter video timing.
    if(mfg::SourceCadenceTrace().Enabled())
        mfg::SourceCadenceTrace().Record("source",qpc,timestamp,REFERENCE_TIME(stream),
            batch.serial,mfg::MaxGenerated,effective,batch.generated,0,false,
            m_mfgLostGenerated.load(std::memory_order_relaxed),0);
    try {
        m_mfgThread.Start([this](int64_t& deadline){return NextMfg(deadline);},[this]{return PresentMfg();});
        m_mfgThread.Wake();return true;
    }catch(...) {StopMfg();return false;}
}
bool CDX11VideoProcessor::PresentMfg() {
    if(!m_pFilter->m_RendererLock.TryLock()) {
        m_mfgLockRetries.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    struct Unlock{CCritSec& lock;~Unlock(){lock.Unlock();}} unlock{m_pFilter->m_RendererLock};
    if(!m_pDXGISwapChain1||m_pFilter->m_filterState!=State_Running) {
        m_mfgPreparedFrame.reset();
        m_mfgPreparedOriginalPending.store(0,std::memory_order_relaxed);
        std::lock_guard guard(m_mfgQueueLock);m_mfgQueue.Clear();m_mfgLiveFlowGroups.Clear();return true;
    }
    const double hz=m_dRefreshPeriodMs>0.0?1000.0/m_dRefreshPeriodMs:0.0;
    const auto qpcFrequency=GetPreciseTicksPerSecond();
    const double leadMs=DisplayPacingEnabled()?m_mfgDisplayPacing.Start(hz):0.0;
    const int64_t leadTicks=int64_t(leadMs*qpcFrequency/1000.0);
    const bool prepare=LiveFlowCoreEnabled() && DisplayPreparationEnabled() && !m_mfgPreparationBroken;
    const int64_t prepareTicks=prepare ? int64_t(mfg::DisplayPreparation::LeadMs(hz)*qpcFrequency/1000.0) : 0;
    const auto now=int64_t(GetPreciseTick());
    std::optional<mfg::QueuedFrame> frame;
    bool alreadyPrepared=false;
    if(m_mfgPreparedFrame) {
        if(!mfg::DisplayPreparation::InSubmitWindow(now,m_mfgPreparedFrame->deadline,leadTicks)) return true;
        frame=std::move(m_mfgPreparedFrame);
        m_mfgPreparedFrame.reset();
        m_mfgPreparedOriginalPending.store(0,std::memory_order_relaxed);
        alreadyPrepared=true;
    } else {
        // Pre-draw a single picture, one refresh interval before its PTS.
        // A non-consuming peek ensures a failed allocation/draw never discards
        // an original picture. Do NOT render directly to the swapchain here:
        // MPC-VR's source thread can still write that back buffer (VSR/legacy).
        if(prepare) {
            std::optional<mfg::QueuedFrame> candidate;
            {
                std::lock_guard guard(m_mfgQueueLock);
                candidate=m_mfgLiveFlowGroups.PeekReady(now,prepareTicks,
                    [this](uint64_t serial,unsigned phase){return m_mfg.Ready(serial,phase);},
                    leadTicks);
            }
            if(candidate) {
                const auto surface=candidate->index==mfg::MaxGenerated
                    ? mfg::FrameSurface{candidate->original ? candidate->original->texture.Get() : nullptr,
                                        candidate->original ? candidate->original->view.Get() : nullptr}
                    : m_mfg.Frame(candidate->serial,candidate->index);
                HRESULT drawn=E_FAIL;
                const int64_t drawStarted=int64_t(GetPreciseTick());
                CComPtr<ID3D11Texture2D> back;
                if(surface.texture && surface.view &&
                   SUCCEEDED(m_pDXGISwapChain1->GetBuffer(0,IID_PPV_ARGS(&back)))) {
                    drawn=EnsureMfgPreparedTexture(back);
                    if(SUCCEEDED(drawn)) drawn=RenderMfgVideo(surface,candidate->nativeInput,m_mfgPreparedTexture);
                }
                if(SUCCEEDED(drawn)) {
                    const int64_t drawFinished=int64_t(GetPreciseTick());
                    if(qpcFrequency>0.0) {
                        m_mfgPrepareLastDrawUs.store(int64_t((drawFinished-drawStarted)*1e6/qpcFrequency),std::memory_order_relaxed);
                        m_mfgPrepareReadyMarginUs.store(int64_t((candidate->deadline-drawFinished)*1e6/qpcFrequency),std::memory_order_relaxed);
                    }
                    mfg::QueueDrops lost;
                    std::optional<mfg::QueuedFrame> accepted;
                    {
                        std::lock_guard guard(m_mfgQueueLock);
                        // Commit using the same selection time as PeekReady.
                        // Drawing may take milliseconds; recomputing 'now'
                        // could skip the selected phase and pop an ORIGINAL
                        // before the staged image has been committed.
                        accepted=m_mfgLiveFlowGroups.TakeDue(now,lost,prepareTicks,
                            [this](uint64_t serial,unsigned phase){return m_mfg.Ready(serial,phase);},
                            leadTicks);
                        if(lost.Total()) {
                            m_mfgSkipped.fetch_add(lost.Total(),std::memory_order_relaxed);
                            m_mfgLostReal.fetch_add(lost.real,std::memory_order_relaxed);
                            m_mfgLostGenerated.fetch_add(lost.generated,std::memory_order_relaxed);
                            m_mfgDroppedDue.fetch_add(lost.Total(),std::memory_order_relaxed);
                        }
                    }
                    if(accepted && accepted->serial==candidate->serial &&
                       accepted->index==candidate->index && accepted->timestamp==candidate->timestamp) {
                        // The D3D11 GPU read is queued into the prepared
                        // texture. Only now can the NGX bank be recycled.
                        if(accepted->index!=mfg::MaxGenerated) m_mfg.MarkConsumed(accepted->serial);
                        m_mfgPreparedFrame=std::move(accepted);
                        m_mfgPreparedOriginalPending.store(m_mfgPreparedFrame->index==mfg::MaxGenerated ? 1u : 0u,std::memory_order_relaxed);
                        m_mfgPrepareCount.fetch_add(1,std::memory_order_relaxed);
                        if(mfg::SourceCadenceTrace().Enabled())
                            mfg::SourceCadenceTrace().Record("prepared",drawFinished,
                                m_mfgPreparedFrame->timestamp,-1,m_mfgPreparedFrame->serial,
                                m_mfgPreparedFrame->index,
                                m_mfgEffectiveMultiplier.load(std::memory_order_relaxed),0,
                                m_mfgPreparedFrame->deadline,true,
                                m_mfgLostGenerated.load(std::memory_order_relaxed),0);
                        return true; // next wake handles the actual Present
                    }
                    // Unexpected schedule change: do not present the wrong
                    // image. RendererLock normally prevents this race.
                    ++m_mfgPrepareFallbacks;
                } else {
                    // Leave the original/group queued and use the existing
                    // direct compose at its deadline. Avoid repeat attempts
                    // after a device/format allocation problem.
                    m_mfgPreparationBroken=true;
                    m_mfgPrepareFallbacks.fetch_add(1,std::memory_order_relaxed);
                }
            }
        }
        // The pre-draw wake may fire before the real submit window. Only
        // the group scheduler uses this early wake; legacy queue semantics
        // must remain identical when the Stage 3 feature is disabled.
        if(prepare) {
            int64_t nextDeadline=now;
            bool hasNext=false;
            {
                std::lock_guard guard(m_mfgQueueLock);
                hasNext=m_mfgLiveFlowGroups.Next(nextDeadline,0);
            }
            if(hasNext && !mfg::DisplayPreparation::InSubmitWindow(now,nextDeadline,leadTicks)) {
                // Preparation was attempted, but the GPU phase may not yet
                // be ready. The early-wake deadline is already in the past:
                // returning true here would spin at 100% CPU. Let the
                // PresentationThread use its bounded 1ms retry instead.
                return false;
            }
        }
        {
            std::lock_guard guard(m_mfgQueueLock);
            mfg::QueueDrops lost;
            frame=LiveFlowCoreEnabled()
                ? m_mfgLiveFlowGroups.TakeDue(int64_t(GetPreciseTick()),lost,leadTicks,
                    [this](uint64_t serial,unsigned phase){return m_mfg.Ready(serial,phase);})
                : m_mfgQueue.TakeDue(int64_t(GetPreciseTick()),lost,leadTicks);
            if(lost.Total()) {
                m_mfgSkipped.fetch_add(lost.Total(),std::memory_order_relaxed);
                m_mfgLostReal.fetch_add(lost.real,std::memory_order_relaxed);
                m_mfgLostGenerated.fetch_add(lost.generated,std::memory_order_relaxed);
                m_mfgDroppedDue.fetch_add(lost.Total(),std::memory_order_relaxed);
            }
        }
    }
    if(!frame) return false;
    const auto lateTicks=int64_t(GetPreciseTick())-frame->deadline;
    if(DisplayPacingEnabled() && qpcFrequency>0.0) {
        const double latenessMs=1000.0*double(lateTicks)/qpcFrequency;
        const double newLead=m_mfgDisplayPacing.Observe(latenessMs,hz);
        m_mfgDisplayLeadUs.store(int64_t(newLead*1000.0),std::memory_order_relaxed);
        m_mfgDisplayLastLateUs.store(int64_t(latenessMs*1000.0),std::memory_order_relaxed);
        m_mfgDisplayP95NeedUs.store(int64_t(m_mfgDisplayPacing.P95NeedMs()*1000.0),std::memory_order_relaxed);
        m_mfgDisplaySamples.store(unsigned(m_mfgDisplayPacing.Samples()),std::memory_order_relaxed);
    }
    const auto threshold=int64_t(qpcFrequency/500);
    if(lateTicks>threshold) {
        if(frame->index==mfg::MaxGenerated) m_mfgPresentedLateReal.fetch_add(1,std::memory_order_relaxed);
        else m_mfgPresentedLateGenerated.fetch_add(1,std::memory_order_relaxed);
    }
    const auto composeStart=int64_t(GetPreciseTick());
    HRESULT presentHr=E_FAIL;
    if(alreadyPrepared) {
        presentHr=FinishMfgPresent(m_mfgPreparedTexture,frame->timestamp);
    } else {
        const auto surface=frame->index==mfg::MaxGenerated
            ? mfg::FrameSurface{frame->original ? frame->original->texture.Get() : nullptr,
                                frame->original ? frame->original->view.Get() : nullptr}
            : m_mfg.Frame(frame->serial,frame->index);
        if(!surface.texture || !surface.view) {
            if(surface.pending) m_mfgGpuNotReady.fetch_add(1,std::memory_order_relaxed);
            m_mfgDroppedInvalid.fetch_add(1,std::memory_order_relaxed);
            m_mfgSkipped.fetch_add(1,std::memory_order_relaxed);
            if(frame->index==mfg::MaxGenerated)m_mfgLostReal.fetch_add(1,std::memory_order_relaxed);
            else m_mfgLostGenerated.fetch_add(1,std::memory_order_relaxed);
            if(mfg::SourceCadenceTrace().Enabled())
                mfg::SourceCadenceTrace().Record("invalid_surface",int64_t(GetPreciseTick()),
                    frame->timestamp,-1,frame->serial,frame->index,
                    m_mfgEffectiveMultiplier.load(std::memory_order_relaxed),0,
                    frame->deadline,false,
                    m_mfgLostGenerated.load(std::memory_order_relaxed),long(E_FAIL));
            return true;
        }
        presentHr=CompositeMfg(surface,frame->timestamp,frame->nativeInput);
        if(frame->index!=mfg::MaxGenerated) m_mfg.MarkConsumed(frame->serial);
    }
    const auto composeEnd=int64_t(GetPreciseTick());
    if(composeEnd>composeStart)m_mfgComposeTicks.fetch_add(uint64_t(composeEnd-composeStart),std::memory_order_relaxed);
    m_mfgComposeCount.fetch_add(1,std::memory_order_relaxed);
    if(FAILED(presentHr)) {
        std::lock_guard guard(m_mfgQueueLock);m_mfgQueue.Clear();m_mfgLiveFlowGroups.Clear();
        m_mfgPresentFailed=true;
    } else if(presentHr==S_OK) {
        if(frame->index==mfg::MaxGenerated)++m_mfgRealPresents;
        else ++m_mfgGeneratedPresents;
    }
    if(mfg::SourceCadenceTrace().Enabled())
        mfg::SourceCadenceTrace().Record("present_call_return",int64_t(GetPreciseTick()),
            frame->timestamp,-1,frame->serial,frame->index,
            m_mfgEffectiveMultiplier.load(std::memory_order_relaxed),0,
            frame->deadline,alreadyPrepared,
            m_mfgLostGenerated.load(std::memory_order_relaxed),long(presentHr));
    return true;
}

HRESULT CDX11VideoProcessor::EnsureMfgPreparedTexture(ID3D11Texture2D* referenceBackBuffer) {
    if(!referenceBackBuffer) return E_POINTER;
    D3D11_TEXTURE2D_DESC desc{};
    referenceBackBuffer->GetDesc(&desc);
    if(m_mfgPreparedTexture && desc.Width==m_mfgPreparedDesc.Width &&
       desc.Height==m_mfgPreparedDesc.Height && desc.Format==m_mfgPreparedDesc.Format &&
       desc.SampleDesc.Count==m_mfgPreparedDesc.SampleDesc.Count) return S_OK;
    desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags=0;
    desc.MiscFlags=0;
    CComPtr<ID3D11Texture2D> target;
    const HRESULT hr=m_pDevice->CreateTexture2D(&desc,nullptr,&target);
    if(FAILED(hr)) return hr;
    m_mfgPreparedDesc=desc;
    m_mfgPreparedTexture=target;
    return S_OK;
}

HRESULT CDX11VideoProcessor::RenderMfgVideo(const mfg::FrameSurface& surface,bool nativeInput,ID3D11Texture2D* target) {
    if(!surface.texture || !surface.view || !target) return E_POINTER;
    Tex2D_t texture;texture.pTexture=surface.texture;texture.pShaderResource=surface.view;
    surface.texture->GetDesc(&texture.desc);
    D3D11_TEXTURE2D_DESC presentDesc{};target->GetDesc(&presentDesc);
    const CRect sourceRect(0,0,texture.desc.Width,texture.desc.Height);
    const CRect displayRect(0,0,presentDesc.Width,presentDesc.Height);
    if(nativeInput) {
        CComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hr=m_pDevice->CreateRenderTargetView(target,nullptr,&rtv);
        if(FAILED(hr)) return hr;
        const FLOAT black[4]={0,0,0,1};
        m_pDeviceContext->ClearRenderTargetView(rtv,black);
        if(m_mfgLastPostVsrInput && m_mfgPostVsrReady && !m_mfgPostVsrBroken) {
            ID3D11Texture2D* vpInput=surface.texture;
            if(texture.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                // NGX-generated frames are RGBA, while real snapshots are
                // BGRA. One independent VP must see both frame types in order.
                // Convert only generated frames, at native resolution.
                hr=m_mfgPostVsrBGRA.CheckCreate(m_pDevice,
                    DXGI_FORMAT_B8G8R8A8_UNORM,texture.desc.Width,texture.desc.Height,
                    Tex2D_DefaultShaderRTarget);
                if(SUCCEEDED(hr)) hr=TextureCopyRect(texture,m_mfgPostVsrBGRA.pTexture,
                    sourceRect,sourceRect,m_pPS_Simple,nullptr,0,false);
                if(SUCCEEDED(hr)) vpInput=m_mfgPostVsrBGRA.pTexture;
            } else if(texture.desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                hr=E_INVALIDARG;
            }
            if(SUCCEEDED(hr)) {
                // Avoid input/output render-target hazards after the RGBA draw.
                m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
                hr=m_mfgPostVsrVP.ProcessExternal(vpInput,0,target,sourceRect,m_videoRect);
            }
            if(SUCCEEDED(hr)) {
                ++m_mfgPostVsrProcessed;
                return hr;
            }
            ++m_mfgPostVsrFallback;
            if(!m_mfgPostVsrBroken) {
                DLog(L"Hybrid post-MFG VSR processing failed: {}; falling back to Shader and then Golden",HR2Str(hr));
                m_mfgPostVsrBroken=true;
            }
            // Current prepared frame must still be displayed. Future source
            // frames return to the existing VSR-before-MFG Golden path.
            m_pDeviceContext->ClearRenderTargetView(rtv,black);
        }
        return ResizeShaderPass(texture,target,sourceRect,m_videoRect,0);
    }
    const bool needsUpscale=texture.desc.Width!=presentDesc.Width||texture.desc.Height!=presentDesc.Height;
    return TextureCopyRect(texture,target,sourceRect,displayRect,m_pPS_Simple,nullptr,0,false,needsUpscale);
}

HRESULT CDX11VideoProcessor::FinishMfgPresent(ID3D11Texture2D* preparedVideo,REFERENCE_TIME timestamp) {
    CComPtr<ID3D11Texture2D> back;
    HRESULT hr=m_pDXGISwapChain1->GetBuffer(0,IID_PPV_ARGS(&back));
    if(FAILED(hr))return hr;
    if(preparedVideo && back.p != preparedVideo) {
        // A prepared target may still be bound as an RTV from the draw step.
        // Unbind it before using it as the source of a resource copy.
        m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
        // This copy is ordered after the early GPU draw on the same D3D11
        // immediate context. No CPU fence wait is needed for the staged image.
        m_pDeviceContext->CopyResource(back,preparedVideo);
    }
    const auto saved=m_rtStart;m_rtStart=timestamp;
    DrawSubtitles(back);m_rtStart=saved;
    if(m_bShowStats)DrawStats(back);
    if(m_bAlphaBitmapEnable) {
        D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);
        D3D11_VIEWPORT viewport{m_AlphaBitmapNRectDest.left*desc.Width,m_AlphaBitmapNRectDest.top*desc.Height,
            (m_AlphaBitmapNRectDest.right-m_AlphaBitmapNRectDest.left)*desc.Width,
            (m_AlphaBitmapNRectDest.bottom-m_AlphaBitmapNRectDest.top)*desc.Height,0,1};
        AlphaBlt(m_TexAlphaBitmap.pShaderResource,back,m_pAlphaBitmapVertex,&viewport,m_pSamplerLinear);
    }
    if(m_showFeatureBadges.load(std::memory_order_relaxed)) DrawFeatureBadges(back);
    // Both MFG processing orders finish with the existing D3D11 Golden presenter.
    g_bPresent=true;
    hr=m_pDXGISwapChain1->Present(1u,0);
    g_bPresent=false;
    return hr;
}

HRESULT CDX11VideoProcessor::CompositeMfg(const mfg::FrameSurface& surface,REFERENCE_TIME timestamp,bool nativeInput) {
    CComPtr<ID3D11Texture2D> back;
    HRESULT hr=m_pDXGISwapChain1->GetBuffer(0,IID_PPV_ARGS(&back));
    if(FAILED(hr))return hr;
    hr=RenderMfgVideo(surface,nativeInput,back);
    if(FAILED(hr))return hr;
    return FinishMfgPresent(back,timestamp);
}

int CDX11VideoProcessor::GetRenderAhead() {
    if(m_mfgMultiplier&&!m_mfg.Failed()&&!m_mfgPresentFailed) {
        // Optional A/B switch for the previous one-frame render-ahead policy.
        static const bool extendedAhead = [] {
            wchar_t setting[8]{};
            return GetEnvironmentVariableW(L"MPCVR_MFG_RENDER_AHEAD",setting,_countof(setting)) == 0 || setting[0] != L'0';
        }();
        const REFERENCE_TIME duration = m_pFilter->m_FrameStats.GetAverageFrameDuration();
        // Keep the existing 1.5-frame experimental lookahead, but now three
        // banks can retain the earlier pictures while MFG gets ahead.
        // Disable extended lookahead with MPCVR_MFG_RENDER_AHEAD=0.
        const REFERENCE_TIME ahead = extendedAhead ? duration * 3 / 2 + 100000 : duration + 100000;
        const REFERENCE_TIME adaptive = extendedAhead && AdaptiveAheadEnabled()
            ? m_mfgExtraAheadTicks.load(std::memory_order_relaxed) : 0;
        return int(std::clamp<REFERENCE_TIME>(ahead+adaptive,100000,2500000));
    }
    return 0;
}
