// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Helper.h"
#include "DX11VideoProcessor.h"
// No NR/SR/Plus enhancement shared header is needed.
#include "VideoRenderer.h"
#include "Mfg/DisplayCadence.h"
#include "Times.h"
#include "Mfg/CadenceTrace.h"
#include <algorithm>
#include <limits>
#include <vector>
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

bool CDX11VideoProcessor::MfgPostUpscaleRequested() const {
    const auto height = ProcessingSourceRect(CanProjectJvr()).Height();
    if (!UpscaleAt(UpscaleSlot::AfterMfg) || height <= 0) return false;
    // If the user manually caps MPCVR_MFG_MAX_HEIGHT, the dedicated
    // post upscaler requires real and generated frames to stay within
    // that same source-size envelope. The default path is uncapped.
    wchar_t text[16]{};
    const DWORD n=GetEnvironmentVariableW(L"MPCVR_MFG_MAX_HEIGHT",text,_countof(text));
    UINT cap=std::numeric_limits<UINT>::max();
    if(n>0 && n<_countof(text)) {
        wchar_t* end=nullptr;
        const unsigned long value=std::wcstoul(text,&end,10);
        if(end!=text && *end==L'\0') {
            if(value==0) cap=std::numeric_limits<UINT>::max();
            else if(value>=360 && value<=8192) cap=UINT(value);
        }
    }
    return UINT(height) <= cap;
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
    if (!UpscaleAt(UpscaleSlot::AfterMfg) || m_mfgPostVsrBroken || !m_D3D11VP.IsReady() ||
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

void CDX11VideoProcessor::DiscardMfgPreparedFrame() {
    m_mfgPreparedFrame.reset();
    m_mfgPreparedOriginalPending.store(0,std::memory_order_relaxed);
    m_mfgPreparedTexture.Release();
    m_mfgPreparedDesc = {};
    m_mfgPreparationBroken = false;
}

void CDX11VideoProcessor::SwitchMfgUpscaleSlot(unsigned requestedMode) {
    // Configure() is called with RendererLock held. The presentation worker
    // uses TryLock and therefore cannot be joined while inside a draw here.
    // Stop it without calling StopMfg(): that would discard queued originals.
    m_mfgThread.Stop();

    struct PendingSource {
        mfg::QueuedFrame frame;
        bool prepared = false;
    };
    std::vector<PendingSource> pending;
    unsigned discardedGenerated = 0;
    if (m_mfgPreparedFrame) {
        if (m_mfgPreparedFrame->index == mfg::MaxGenerated)
            pending.push_back({*m_mfgPreparedFrame, true});
        else
            ++discardedGenerated;
    }
    {
        std::lock_guard guard(m_mfgQueueLock);
        discardedGenerated += m_mfgLiveFlowGroups.PendingGenerated() + m_mfgQueue.PendingGenerated();
        // Only one scheduler is active in any session. Query both so the
        // optional Live Flow rollback mode also preserves source snapshots.
        for (const auto& original : m_mfgLiveFlowGroups.PendingOriginalFrames())
            pending.push_back({original, false});
        for (const auto& original : m_mfgQueue.PendingOriginalFrames())
            pending.push_back({original, false});
    }
    std::stable_sort(pending.begin(), pending.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.frame.timestamp < rhs.frame.timestamp;
    });

    // Complete the old route before changing its VSR/NGX state. The source
    // snapshots do not reference recyclable DLSSG banks. Generated phases
    // are intentionally skipped at this reconfiguration boundary.
    unsigned presented = 0;
    unsigned failed = 0;
    for (const auto& source : pending) {
        HRESULT hr = E_FAIL;
        if (m_pDXGISwapChain1 && m_pFilter->m_filterState == State_Running &&
            !m_videoRect.IsRectEmpty()) {
            if (source.prepared) {
                hr = FinishMfgPresent(m_mfgPreparedTexture, source.frame.timestamp, source.frame.serial);
            } else if (source.frame.original && source.frame.original->texture && source.frame.original->view) {
                const mfg::FrameSurface surface{source.frame.original->texture.Get(),
                                                source.frame.original->view.Get()};
                // Drain uses a plain display pass: do not evaluate NGX
                // against a soon-to-be-reset temporal history. This is also
                // safe when switching AWAY from a failing post-SR route.
                CComPtr<ID3D11Texture2D> back;
                hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&back));
                if (SUCCEEDED(hr))
                    hr = RenderMfgVideo(surface, source.frame.nativeInput, back,
                                        source.frame.timestamp, PictureForPlayback(CanProjectJvr()), m_mfgLastVideoOnlyInput, true);
                if (SUCCEEDED(hr))
                    hr = FinishMfgPresent(back, source.frame.timestamp, source.frame.serial);
            }
        }
        if (SUCCEEDED(hr)) {
            ++presented;
            m_mfgRealPresents.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++failed;
        }
    }
    if (!pending.empty() || discardedGenerated)
        DLog(L"MFG live upscale switch: {} original frames presented ({} failed), {} generated phases skipped",
            presented, failed, discardedGenerated);
    if (discardedGenerated) {
        m_mfgSkipped.fetch_add(discardedGenerated, std::memory_order_relaxed);
        m_mfgLostGenerated.fetch_add(discardedGenerated, std::memory_order_relaxed);
    }
    if (failed)
        m_mfgLostReal.fetch_add(failed, std::memory_order_relaxed);

    // One active route and one NGX SR feature; no simultaneous pre/post AI.
    // The presenter is already stopped and the old originals are retired.
    StopMfg(true);
    m_dlssSR.ResetHistory();
    m_mfgProcessingMode = requestedMode;
    ResetFeatureStatus();
}

void CDX11VideoProcessor::ResetMfgFrameAnalysis() {
    m_mfgDecodedDifference.Reset();
    m_mfgPendingRepeats.store(0, std::memory_order_relaxed);
    m_mfgRepeatHold = false;
    m_mfgLowMotionActive = false;
    m_mfgLowMotionStreak = 0;
    m_mfgQuietFlowCredit = 0;
    m_mfgQuietFlowPts = -1;
    m_mfgOriginalPrime.reset();
}

bool CDX11VideoProcessor::MfgDepthSourceEligible() const {
    return !SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_Dovi.bValid &&
        !m_iRotation && !m_bFlip && m_srcRectWidth > 0 && m_srcRectHeight > 0;
}

bool CDX11VideoProcessor::MfgSourceMeetsDisplayRate() const {
    return m_mfgDisplayRate.Bypassed(m_rtAvgTimePerFrame, m_dRefreshPeriodMs, m_bDoubleFrames);
}

bool CDX11VideoProcessor::MfgFrameAnalysisEligible() const {
    return m_mfgMultiplier == mfg::AutoMultiplier && !MfgSourceMeetsDisplayRate() &&
        m_pFilter->m_filterState == State_Running && !m_pFilter->m_bClockStalled &&
        !m_bInterlaced && m_SampleFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE &&
        !SourceIsHDR() && !m_Dovi.bValid && !m_pPSHalfOUtoInterlace &&
        (CanProjectJvr() || m_srcDXGIFormat == DXGI_FORMAT_NV12 || m_srcDXGIFormat == DXGI_FORMAT_P010);
}

std::wstring CDX11VideoProcessor::MfgFrameAnalysisStatus() const {
    const bool available = MfgFrameAnalysisEligible();
    const wchar_t* state = !available ? L"unavailable" : m_mfgRepeatHold ? L"holding still picture" : L"detecting";
    auto status = std::format(L"\nMFG repeats    : {} ({} repeats bypassed)", state, m_mfgRepeatsBypassed);
    if (m_mfgMultiplier == mfg::AutoMultiplier) {
        const auto slots = m_mfgRepeatTiming.Slots();
        status += slots ? std::format(L"\nMFG timing     : {} refresh slots", slots)
                        : L"\nMFG timing     : source timing";
        if (const auto period = m_mfgRepeatTiming.Period(); slots && period > 0)
            status += std::format(L" (repeat smoothing, {:.2f} ms mean)", double(period) / 10000.0);
    }
    if (m_mfgLowMotionEnabled) {
        const wchar_t* state = !available ? L"unavailable" : m_mfgLowMotionActive ? L"saving" : L"watching";
        status += std::format(L"\nMFG low motion : {} ({} saved, {} flow vetoes)",
            state, m_mfgLowMotionSaved, m_mfgLowMotionVetoes);
    }
    return status;
}

void CDX11VideoProcessor::StopMfg(bool release, bool fastShutdown) {
    ResetMfgFrameAnalysis();
    m_mfgLastSourceColorFinal.reset();
    m_mfgLastToneBeforeUpscale=false;
    m_mfgRepeatTiming.Reset();
    if (release) m_mfgDecodedDifference = {};
    // Presenter never blocks on RendererLock, so joining under it is safe.
    m_mfgThread.Stop();
    if (m_dlssSRAfterMfg) {
        m_dlssSR.ResetHistory();
        m_dlssSRAfterMfg = false;
    }
    DiscardMfgPreparedFrame();
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
    m_mfgTargetSlots = 0;
    m_mfgBudgetCandidate=0;
    m_mfgBudgetStreak=0;
    if(release) {
        m_mfgOriginalsReplaced=0;
        if (fastShutdown) m_mfg.FastShutdown();
        else m_mfg.Reset();
        m_mfgPresentFailed=false;m_mfgInputModeKnown=false;
        m_mfgLastPostVsrInput=false;
        m_mfgLastVideoOnlyInput = false;
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
    return LiveFlowCoreEnabled() ? m_mfgLiveFlowGroups.Next(deadline,std::max(leadTicks,prepareTicks),leadTicks)
                                 : m_mfgQueue.Next(deadline,leadTicks);
}
bool CDX11VideoProcessor::QueueMfg(ID3D11Texture2D* image, REFERENCE_TIME timestamp, bool& overlayInputHandled,
    bool nativeInput, bool postVsrInput, bool videoOnlyInput, bool projected) {
    // Source and NGX GPU producer use the same playback cancellation ticket.
    const auto ticket = m_pFilter->m_playbackCancel.Capture();
    if (ticket->Cancelled()) return false;
    m_mfg.SetCancellation(ticket);
    if(m_mfgThread.Failed())m_mfgPresentFailed=true;
    if(m_mfgPresentFailed) {StopMfg(false);return false;}
    if(m_mfg.Failed())return false;
    const unsigned requested=m_mfgMultiplier.load(std::memory_order_relaxed);
    const double refreshHz=m_dRefreshPeriodMs>0.0 ? 1000.0/m_dRefreshPeriodMs : 0.0;
    const auto inputPeriod=m_pFilter->m_FrameStats.GetAverageFrameDuration();
    const double sourceFps=inputPeriod>0 ? 1e7/double(inputPeriod) : 0.0;
    const bool autoMode=requested==mfg::AutoMultiplier;
    const unsigned maxMultiplier = m_mfg.MaxAvailableMultiplier();
    const bool routeChanged = m_mfgInputModeKnown && (m_mfgLastNativeInput != nativeInput ||
        m_mfgLastPostVsrInput != postVsrInput || m_mfgLastVideoOnlyInput != videoOnlyInput);
    const Tex2D_t* sourceToneInput = nullptr;
    const bool upstreamTone=m_adaptiveTone.sourceToneBeforeUpscale;
    if(!upstreamTone && SourceToneEligible()) {
        const auto* raw = nativeInput ? (projected ? &m_jvrFrame : &m_mfgNativeInput) :
            (m_completedTarget->image.pTexture.p == image ? &m_completedTarget->image : &m_processingTexture);
        if(raw->pTexture.p == image && raw->pShaderResource) sourceToneInput=raw;
    }
    const bool sourceColorFinal = upstreamTone || sourceToneInput != nullptr;
    const bool colorRouteChanged = m_mfgLastSourceColorFinal.has_value() &&
        (*m_mfgLastSourceColorFinal != sourceColorFinal || m_mfgLastToneBeforeUpscale != upstreamTone);
    if(colorRouteChanged) m_mfg.ResetHistory(); // same existing cut-style reset, no bank retirement
    const bool checkFrames = MfgFrameAnalysisEligible() && !routeChanged && !colorRouteChanged;
    const auto priorOriginalPts = m_mfgAutoLastPts;
    bool repeatBridge = false;
    unsigned repeats = 0;
    bool timingEligible = false;
    bool resumeAfterSaving = false;
    bool lowMotionCandidate = false;
    if (checkFrames) {
        const auto difference = m_mfgDecodedDifference.Take(m_pDeviceContext, timestamp, m_mfgLowMotionEnabled);
        if (mfg::SourceCadenceTrace().Enabled()) {
            mfg::SourceCadenceTrace().Record("repeat_compare", GetPreciseTick(), timestamp, -1,
                0, unsigned(difference), 0, 0, 0, false, 0, m_mfgDecodedDifference.ReadbackResult());
        }
        const bool comparable = priorOriginalPts >= 0 && timestamp > priorOriginalPts;
        const bool duplicate = comparable && difference == mfg::FrameDifference::Duplicate;
        const bool changed = difference == mfg::FrameDifference::Different || difference == mfg::FrameDifference::LowMotion;
        // Bound the window for bridging repeats. After a long hold, keep
        // presenting the still original so subtitles and OSD can advance.
        const auto duration = std::clamp<REFERENCE_TIME>(
            inputPeriod, 0, 2'500'000);
        // Allow jitter around the estimated duration, including alternating
        // short and long VFR samples at the end of a confirmed repeat run.
        const auto bridgeLimit = std::clamp<REFERENCE_TIME>(duration * 3 + duration / 2, 800'000, 2'500'000);
        const auto span = timestamp - priorOriginalPts;
        const auto nextFrameDuration = m_sampleDuration > 0
            ? std::min<REFERENCE_TIME>(m_sampleDuration, 2'500'000) : duration;
        const bool shortSpan = span + nextFrameDuration <= bridgeLimit;
        const bool repeatFits = duplicate && !m_mfgRepeatHold && shortSpan &&
            mfg::RepeatFitsDisplayBudget(priorOriginalPts, timestamp, nextFrameDuration,
                m_mfgAutoEpoch, refreshHz, maxMultiplier);
        if (repeatFits && !m_completingSample) {
            m_mfgPendingRepeats.fetch_add(1, std::memory_order_relaxed);
            ++m_mfgRepeatsBypassed;
            if (mfg::SourceCadenceTrace().Enabled()) {
                mfg::SourceCadenceTrace().Record("repeat_bypassed", GetPreciseTick(), timestamp, -1,
                    0, mfg::MaxGenerated, 0, 0, 0, false, 0, 0);
            }
            return true;
        }

        repeats = m_mfgPendingRepeats.exchange(0, std::memory_order_relaxed);
        const bool repeatPending = repeats != 0;
        // A busy readback cannot classify this sample, but the preceding
        // repeats were already confirmed. Keep this sample as an original and
        // preserve NGX history across the bounded gap, as on a normal frame
        // with a busy readback. A classified cut still resets history.
        const bool readbackBusy = difference == mfg::FrameDifference::Unknown &&
            m_mfgDecodedDifference.ReadbackResult() == DXGI_ERROR_WAS_STILL_DRAWING;
        // A repeat retained for the budget can close a pending gap. Keep the
        // same cadence rules for that shorter span.
        repeatBridge = repeatPending && (changed || readbackBusy || duplicate) && span > 0 && span <= bridgeLimit;
        resumeAfterSaving = m_mfgLowMotionActive && changed && comparable && !repeatPending && span <= 2'500'000;
        lowMotionCandidate = m_mfgLowMotionEnabled && comparable && !repeatPending && !duplicate &&
            (difference == mfg::FrameDifference::LowMotion || difference == mfg::FrameDifference::Duplicate);
        constexpr unsigned EnterSavingAfter = 3;
        constexpr unsigned QuietFlowCredits = 3;
        constexpr int64_t MaxQuietFlowAge = 1'250'000; // 125 ms
        m_mfgLowMotionStreak = lowMotionCandidate ? std::min(m_mfgLowMotionStreak + 1, EnterSavingAfter) : 0;
        if (lowMotionCandidate) {
            const auto flow = m_mfg.TakeFlowMotion(priorOriginalPts);
            if (flow == mfg::FlowMotion::Quiet) {
                m_mfgQuietFlowCredit = QuietFlowCredits;
                m_mfgQuietFlowPts = priorOriginalPts;
            } else if (flow == mfg::FlowMotion::Active) {
                m_mfgQuietFlowCredit = 0;
                ++m_mfgLowMotionVetoes;
            }
            if (flow != mfg::FlowMotion::Unknown && mfg::SourceCadenceTrace().Enabled()) {
                mfg::SourceCadenceTrace().Record(flow == mfg::FlowMotion::Quiet ? "low_motion_flow_quiet" : "low_motion_flow_veto",
                    GetPreciseTick(), timestamp, -1, 0, mfg::MaxGenerated, 0, 0, 0, false, 0, 0);
            }
        }
        if (!lowMotionCandidate || timestamp - m_mfgQuietFlowPts > MaxQuietFlowAge)
            m_mfgQuietFlowCredit = 0;
        m_mfgLowMotionActive = m_mfgLowMotionStreak == EnterSavingAfter && m_mfgQuietFlowCredit > 0;
        if (difference == mfg::FrameDifference::Cut) {
            if (UsesZipDepthSr() || UsesZipDepthMfg()) m_zipDepth.MarkSceneCut(timestamp);
        }
        if (difference == mfg::FrameDifference::Cut || m_mfgRepeatHold || (repeatPending && !repeatBridge)) {
            m_mfg.ResetHistory();
        }
        // A repeat kept for lack of generation capacity still occupies its
        // original slot. Keep interpolation active for the adjoining spans.
        m_mfgRepeatHold = duplicate && (m_mfgRepeatHold || !shortSpan);
        timingEligible = difference != mfg::FrameDifference::Cut &&
            !duplicate && (!repeatPending || repeatBridge) && !m_mfgLowMotionActive && !m_mfgRepeatHold;
        m_mfgDecodedDifference.KeepCurrent(timestamp);
    } else {
        if (m_mfgPendingRepeats.load(std::memory_order_relaxed) || m_mfgRepeatHold) m_mfg.ResetHistory();
        ResetMfgFrameAnalysis();
    }
    if (autoMode) {
        // Do not treat a speed change or seek as one enormous interpolation interval.
        if(m_mfgAutoLastPts<0 || timestamp<=m_mfgAutoLastPts ||
           timestamp-m_mfgAutoLastPts>2500000) {
            m_mfgAutoEpoch=timestamp;
            m_mfgAutoLastPts=-1;
        }
    }
    const auto timing = m_mfgRepeatTiming.Observe(m_mfgAutoLastPts, timestamp,
        inputPeriod, repeats, timingEligible, autoMode ? m_mfgAutoEpoch : -1, refreshHz, maxMultiplier);
    const unsigned targetSlots = !autoMode ? 0 : timing.slots ? timing.slots
        : mfg::AutoDisplaySlots(timing.Start(m_mfgAutoLastPts),timing.End(timestamp),m_mfgAutoEpoch,refreshHz);
    const unsigned proposed=autoMode
        ? std::min(targetSlots ? targetSlots : 2u, maxMultiplier)
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
    if (routeChanged) {
        StopMfg(false);
        m_mfg.ResetHistory();
        m_mfgRepeatTiming.Observe(-1, timestamp, inputPeriod, 0, false, m_mfgAutoEpoch, refreshHz, maxMultiplier);
    }
    m_mfgInputModeKnown=true;
    m_mfgLastNativeInput=nativeInput;
    m_mfgLastPostVsrInput=postVsrInput;
    m_mfgLastVideoOnlyInput = videoOnlyInput;
    const bool savedEvaluation = m_mfgLowMotionActive && effective >= 2;
    if (savedEvaluation) --m_mfgQuietFlowCredit;
    if (m_mfgRepeatHold || m_mfgLowMotionActive) {
        effective = 1;
        m_mfgEffectiveMultiplier.store(effective, std::memory_order_relaxed);
    }
    m_mfgTargetSlots = targetSlots;
    // The source snapshot excludes subtitles and OSD. Full-backbuffer
    // input needs a picture rect so black bars are excluded from estimation.
    std::optional<mfg::DepthInput> depth;
    const bool useDepthForMfg = UsesZipDepthMfg() && MfgDepthSourceEligible();
    // After-MFG SR needs a depth estimate from the original frame, even
    // when MFG itself uses flat depth.
    const auto frameSource = ProcessingSourceRect(projected);
    const bool captureForSr = !projected && nativeInput && UsesZipDepthSr() && MfgPostUpscaleRequested() &&
        CanDlssSRUpscale(frameSource, ProcessingRect());
    const auto displayPicture = PictureForPlayback(projected);
    const auto& outputPicture = m_completingSample && m_completedTarget ? m_completedTarget->picture : displayPicture;
    RECT sourcePicture = outputPicture;
    if (nativeInput || videoOnlyInput) {
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        sourcePicture = {0, 0, LONG(desc.Width), LONG(desc.Height)};
    }
    if (useDepthForMfg || captureForSr) {
        if (projected || nativeInput || videoOnlyInput || m_renderRect == m_videoRect) {
            const SIZE modelSize{frameSource.Width(), frameSource.Height()};
            depth.emplace(mfg::DepthInput{m_zipDepth, sourcePicture, modelSize, !projected && !overlayInputHandled,
                useDepthForMfg, m_adaptiveMfgDepth});
        }
    }

    // [FA-03A | 2026-11-25 14:57 +08:00] Independent AT only, on
    // Completed originals. Depth and decoded difference remain raw. The
    // same D3D11 context copies the corrected texture into MFG's owned bank.
    // FA-03B: DLSS SR already received the original-size graded image;
    // its depth capture also used the ungraded original. Do not repeat either.
    if(upstreamTone && depth) depth->captureSource=false;
    if(sourceToneInput) {
        auto* corrected=PrepareSourceTone(*sourceToneInput, CRect(sourcePicture), timestamp);
        if(corrected != image) {
            // Preserve the existing raw depth input and avoid capturing
            // again from the corrected MFG snapshot.
            if(depth && depth->captureSource) {
                m_zipDepth.Update(m_pDevice,m_pDeviceContext,sourceToneInput->pShaderResource,
                    depth->picture,depth->modelSize,timestamp);
                depth->captureSource=false;
            }
            image=corrected;
        }
    }
    mfg::Batch batch;
    const auto generateStart=int64_t(GetPreciseTick());
    bool generated;
    if (effective < 2) {
        generated = m_mfg.CaptureOriginal(image, timestamp, batch, depth ? &*depth : nullptr);
    } else {
        bool primed = true;
        if ((repeatBridge || resumeAfterSaving) && m_mfgOriginalPrime) {
            // The original-only path did not update NGX. Restore the last
            // retained source only when interpolation is needed again.
            mfg::Batch anchor;
            primed = m_mfg.Generate(m_pDevice, m_pDeviceContext, m_mfgOriginalPrime->texture.Get(),
                2, priorOriginalPts, anchor);
        }
        // Retry on a normal source interval, where a refusal cannot leave a
        // gap from an already bypassed repeat.
        const unsigned requestedMultiplier = autoMode && !repeatBridge
            ? std::min(std::max(effective, targetSlots), mfg::MaxMultiplier) : effective;
        const mfg::FrameGeometry geometry{sourcePicture, frameSource, displayPicture};
        generated = primed && m_mfg.Generate(m_pDevice, m_pDeviceContext, image,
            requestedMultiplier, timestamp, batch, false, lowMotionCandidate, depth ? &*depth : nullptr,
            &geometry);
        if (generated) {
            effective = std::min(requestedMultiplier, m_mfg.MaxAvailableMultiplier());
            m_mfgEffectiveMultiplier.store(effective, std::memory_order_relaxed);
        }
    }
    m_mfgOriginalPrime = checkFrames && effective < 2 && !m_mfgRepeatHold ? batch.original : nullptr;
    const auto generateEnd=int64_t(GetPreciseTick());
    if (generateEnd>generateStart) m_mfgGenerateTicks.fetch_add(uint64_t(generateEnd-generateStart),std::memory_order_relaxed);
    m_mfgGenerateCount.fetch_add(1,std::memory_order_relaxed);
    if(!generated) {
        StopMfg(false);DLog(L"MFG unavailable: {}",m_mfg.Status());return false;
    }
    m_mfgLastSourceColorFinal=sourceColorFinal;
    m_mfgLastToneBeforeUpscale=upstreamTone;
    // The timing of an interpolated interval belongs to this pair only.
    // NGX can report another start after its own history reset.
    const mfg::PresentationTiming batchTiming{
        batch.previous == priorOriginalPts ? timing.previous : -1, timing.current,
        batch.previous == priorOriginalPts ? timing.slots : 0};
    CRefTime stream;if(FAILED(m_pFilter->StreamTime(stream))) {StopMfg();return false;}
    if (savedEvaluation) {
        ++m_mfgLowMotionSaved;
        if (mfg::SourceCadenceTrace().Enabled()) {
            mfg::SourceCadenceTrace().Record("low_motion_saved", GetPreciseTick(), timestamp, stream,
                batch.serial, mfg::MaxGenerated, 1, 0, 0, false, 0, 0);
        }
    }
    wchar_t trace[2]{};
    if(GetEnvironmentVariableW(L"MPCVR_MFG_TRACE",trace,2))
        fprintf(stderr,"MFG_READY stamp=%lld stream=%lld lead_us=%lld generated=%u ahead=%d path=%s\n",timestamp,REFERENCE_TIME(stream),(timestamp-REFERENCE_TIME(stream))/10,batch.generated,GetRenderAhead(),nativeInput ? "source-first" : "legacy");
    const auto qpc=int64_t(GetPreciseTick());
    if (m_completingSample && m_completedTarget) {
        if (mfg::SourceCadenceTrace().Enabled()) {
            mfg::SourceCadenceTrace().Record("completed_mfg_submit", generateEnd, timestamp, stream,
                batch.serial, mfg::MaxGenerated, effective, batch.generated, 0, false,
                m_mfgLostGenerated, S_OK, generateEnd - generateStart);
        }
        const auto lost = m_completedGroups.Add(batch, qpc, stream, GetPreciseTicksPerSecond(),
            m_rtVideoDelay, refreshHz, nativeInput, m_mfg.BankCount(), autoMode,
            DynamicSourceTimingEnabled(), repeatBridge, batchTiming, mfg::LiveFlowGroups::DeadlineMode::StreamTime);
        m_completedStatsDrops.other += lost.generated;
        m_mfgLostGenerated += lost.generated;
        m_mfgSkipped += lost.generated;
        auto phases = m_completedGroups.TakeBufferedGroup();
        if (phases.empty()) return false;
        m_completedTarget->sourceColorFinal=sourceColorFinal;
        m_completedBatches.push_back({std::move(*m_completedTarget), std::move(phases), videoOnlyInput});
        m_completedTarget.reset();
        m_completedPresenter.Wake();
        return true;
    }
    // Manager-inspired work-ahead control, grounded in actual DirectShow PTS:
    // if a generated frame becomes ready too close to its deadline, ask the
    // upstream renderer for a little more read-ahead on *future* samples.
    // Measure lead against the same presentation times that the queue uses.
    if (AdaptiveAheadEnabled() && batch.generated>0 && batch.previous>=0 &&
        batch.current>batch.previous && batch.current-batch.previous<=2500000) {
        const double spanMs=double(batchTiming.End(batch.current)-batchTiming.Start(batch.previous))/10000.0;
        const double leadMs=(double(batchTiming.Phase(batch.previous,batch.current,1,batch.generated+1)) +
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
                DynamicSourceTimingEnabled(), repeatBridge, batchTiming)
            : m_mfgQueue.Add(batch,qpc,stream,GetPreciseTicksPerSecond(),m_rtVideoDelay,
                m_dRefreshPeriodMs>0?1000./m_dRefreshPeriodMs:0.,nativeInput,m_mfg.BankCount(),autoMode,batchTiming);
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
    const mfg::GeneratedReady generatedReady=[this](uint64_t serial,unsigned phase){return m_mfg.Ready(serial,phase);};
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
                candidate=m_mfgLiveFlowGroups.PeekReady(now,prepareTicks,generatedReady,leadTicks);
            }
            // Prefer decides an original's fate in the submit
            // window. Precomposing it would commit a frame one refresh early.
            if(candidate && candidate->index!=mfg::MaxGenerated) {
                const auto surface=m_mfg.Frame(candidate->serial,candidate->index);
                HRESULT drawn=E_FAIL;
                const int64_t drawStarted=int64_t(GetPreciseTick());
                CComPtr<ID3D11Texture2D> back;
                if(surface.texture && surface.view &&
                   SUCCEEDED(m_pDXGISwapChain1->GetBuffer(0,IID_PPV_ARGS(&back)))) {
                    drawn=EnsureMfgPreparedTexture(back);
                    if(SUCCEEDED(drawn)) drawn=RenderMfgVideo(surface,candidate->nativeInput,m_mfgPreparedTexture,
                        candidate->timestamp, PictureForPlayback(CanProjectJvr()), m_mfgLastVideoOnlyInput, false, CanProjectJvr());
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
                        accepted=m_mfgLiveFlowGroups.TakeDue(now,lost,prepareTicks,generatedReady,leadTicks);
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
                    if (m_dlssSRAfterMfg) m_dlssSR.ResetHistory();
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
            const auto submitNow=int64_t(GetPreciseTick());
            const auto refreshTicks=hz>0.0 && qpcFrequency>0.0 ? int64_t(qpcFrequency/hz) : 0;
            frame=LiveFlowCoreEnabled()
                ? m_mfgLiveFlowGroups.TakeReplacement(submitNow,leadTicks,refreshTicks,MfgOriginalProtection,lost,generatedReady)
                : m_mfgQueue.TakeReplacement(submitNow,leadTicks,refreshTicks,MfgOriginalProtection,lost,generatedReady);
            m_mfgOriginalsReplaced+=lost.real;
            if(!frame) {
                frame=LiveFlowCoreEnabled()
                    ? m_mfgLiveFlowGroups.TakeDue(submitNow,lost,leadTicks,generatedReady)
                    : m_mfgQueue.TakeDue(submitNow,lost,leadTicks,generatedReady);
            }
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
        presentHr=FinishMfgPresent(m_mfgPreparedTexture,frame->timestamp,frame->serial,false,nullptr,false,false,frame->index==mfg::MaxGenerated);
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
        presentHr=CompositeMfg(surface,frame->timestamp,frame->nativeInput,frame->serial,frame->index==mfg::MaxGenerated);
        if(frame->index!=mfg::MaxGenerated) m_mfg.MarkConsumed(frame->serial);
    }
    const auto composeEnd=int64_t(GetPreciseTick());
    if(composeEnd>composeStart)m_mfgComposeTicks.fetch_add(uint64_t(composeEnd-composeStart),std::memory_order_relaxed);
    m_mfgComposeCount.fetch_add(1,std::memory_order_relaxed);
    if(FAILED(presentHr)) {
        if(frame->index==mfg::MaxGenerated) ++m_mfgLostReal;
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

HRESULT CDX11VideoProcessor::RenderMfgVideo(const mfg::FrameSurface& surface, bool nativeInput, ID3D11Texture2D* target,
                                            REFERENCE_TIME timestamp, const CRect& targetPicture, bool videoOnlyInput,
                                            bool bypassUpscale, bool projected) {
    if(!surface.texture || !surface.view || !target) return E_POINTER;
    Tex2D_t texture;texture.pTexture=surface.texture;texture.pShaderResource=surface.view;
    surface.texture->GetDesc(&texture.desc);
    D3D11_TEXTURE2D_DESC presentDesc{};target->GetDesc(&presentDesc);
    const CRect sourceRect(0,0,texture.desc.Width,texture.desc.Height);
    const CRect displayRect(0,0,presentDesc.Width,presentDesc.Height);
    if (videoOnlyInput && targetPicture == displayRect && sourceRect == displayRect &&
        texture.desc.Format == presentDesc.Format) {
        m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        m_pDeviceContext->CopyResource(target, surface.texture);
        return S_OK;
    }
    if (nativeInput || videoOnlyInput) {
        CComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hr=m_pDevice->CreateRenderTargetView(target,nullptr,&rtv);
        if(FAILED(hr)) return hr;
        const FLOAT black[4]={0,0,0,1};
        m_pDeviceContext->ClearRenderTargetView(rtv,black);
        if (videoOnlyInput || bypassUpscale) {
            // During a live mode change the old source snapshots are shown
            // without evaluating either AI upscaler; only the chosen routing
            // slot of the NEXT timeline may perform AI processing.
            return ResizeShaderPass(texture, target, sourceRect, targetPicture, 0, false);
        }
        const auto processingRect = m_processingSize.cx > 0 ? ProcessingRect() : targetPicture;
        if(m_mfgLastPostVsrInput && m_mfgPostVsrReady && !m_mfgPostVsrBroken) {
            auto vsrDestination = targetPicture;
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
                if (m_processingSize.cx > 0 && SUCCEEDED(m_processingTexture.CheckCreate(m_pDevice, presentDesc.Format,
                    processingRect.Width(), processingRect.Height(), Tex2D_DefaultShaderRTarget))) {
                    vsrDestination = processingRect;
                    hr = m_mfgPostVsrVP.ProcessExternal(vpInput, 0, m_processingTexture.pTexture, sourceRect, processingRect);
                    if (SUCCEEDED(hr)) {
                        hr = ResizeShaderPass(m_processingTexture, target, processingRect, targetPicture, 0);
                    }
                } else {
                    hr = m_mfgPostVsrVP.ProcessExternal(vpInput, 0, target, sourceRect, targetPicture);
                }
            }
            if(SUCCEEDED(hr)) {
                RecordEditionVsr(sourceRect, vsrDestination, true);
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
        // The native-size MFG path is the post-upscale slot. A frame that
        // used the full-size/legacy route can never enter this branch.
        if (UpscaleAt(UpscaleSlot::AfterMfg) && CanDlssSRUpscale(sourceRect, processingRect, projected)) {
            if (auto* input = PrepareDlssSR(sourceRect, processingRect, true)) {
                const float frameTimeMs = m_dRefreshPeriodMs > 0.0 ? float(m_dRefreshPeriodMs) : 16.7f;
                if (auto* output = EvaluateDlssSR(texture, *input, sourceRect, timestamp, frameTimeMs)) {
                    const CRect outputRect(0, 0, output->desc.Width, output->desc.Height);
                    return ResizeShaderPass(*output, target, outputRect, targetPicture, 0);
                }
            }
        }
        return ResizeShaderPass(texture,target,sourceRect,targetPicture,0);
    }
    const bool needsUpscale=texture.desc.Width!=presentDesc.Width||texture.desc.Height!=presentDesc.Height;
    return TextureCopyRect(texture,target,sourceRect,displayRect,m_pPS_Simple,nullptr,0,false,needsUpscale);
}

void CDX11VideoProcessor::DrawMfgPresentOverlays(ID3D11Texture2D* target, const CRect& picture,
    REFERENCE_TIME timestamp, uint64_t serial)
{
    if (DepthOverlayEnabled()) {
        const auto depth = m_mfg.OverlayDepth(serial, timestamp);
        if (depth.view) {
            DrawDepthView(target, picture, depth.view, depth.size, CRect(depth.picture), 0, true);
            m_mfg.MarkConsumed(serial);
        } else if (!UsesZipDepthMfg()) {
            DrawDepthOverlay(target, picture, timestamp);
        }
    }
    if (m_showMfgMotion) DrawMfgMotion(target, serial, timestamp, picture);
    const auto saved=m_rtStart;m_rtStart=timestamp;
    DrawSubtitles(target, &picture);m_rtStart=saved;
    if(m_bAlphaBitmapEnable) {
        D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);
        D3D11_VIEWPORT viewport{m_AlphaBitmapNRectDest.left*desc.Width,m_AlphaBitmapNRectDest.top*desc.Height,
            (m_AlphaBitmapNRectDest.right-m_AlphaBitmapNRectDest.left)*desc.Width,
            (m_AlphaBitmapNRectDest.bottom-m_AlphaBitmapNRectDest.top)*desc.Height,0,1};
        AlphaBlt(m_TexAlphaBitmap.pShaderResource,target,m_pAlphaBitmapVertex,&viewport,m_pSamplerLinear);
    }
    const LONG featureOffset = DrawFeatureStatus(target);
    if (m_bShowStats) DrawStats(target, featureOffset);
}

HRESULT CDX11VideoProcessor::FinishMfgPresent(ID3D11Texture2D* preparedVideo,REFERENCE_TIME timestamp,uint64_t serial,
    bool completed, const CRect* pictureOverride, bool svdLutHandled, bool alreadyComposited,bool original) {
    InvalidateCompletedBackBuffer();
    const auto picture = pictureOverride ? *pictureOverride : PictureForPlayback(CanProjectJvr());
    const bool trace = completed && mfg::SourceCadenceTrace().Enabled();
    const auto started = trace ? GetPreciseTick() : 0;
    CComPtr<ID3D11Texture2D> back;
    HRESULT hr=m_pDXGISwapChain1->GetBuffer(0,IID_PPV_ARGS(&back));
    if(FAILED(hr))return hr;
    if (!alreadyComposited) {
        if(preparedVideo && back.p != preparedVideo) {
            // A prepared target may still be bound as an RTV from the draw step.
            // Unbind it before using it as the source of a resource copy.
            m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
            // This copy is ordered after the early GPU draw on the same D3D11
            // immediate context. No CPU fence wait is needed for the staged image.
            m_pDeviceContext->CopyResource(back,preparedVideo);
        }
        if (!svdLutHandled) {
            BeginAdaptiveToneLegacy(back,picture,timestamp,original);
            DrawSvdLut(back,picture,timestamp,original);
            DrawAdaptiveToneTarget(back,picture,timestamp);
            m_adaptiveTone.allowDraw=false;
        }
        DrawMfgPresentOverlays(back, picture, timestamp, serial);
    }
    // A DO_NOT_WAIT retry may reuse the exact same already-composited buffer.
    // Repainting overlays there can duplicate alpha blends and keep source
    // Receive waiting on RendererLock after a transient DXGI stall.
    if (trace) {
        const auto finished = GetPreciseTick();
        mfg::SourceCadenceTrace().Record("completed_osd", finished, timestamp, -1,
            serial, 0, 0, 0, 0, true, m_mfgLostGenerated, S_OK, finished - started);
    }
    const auto presentStarted = trace ? GetPreciseTick() : 0;
    // The completed-frame presenter already schedules against the display clock.
    // Never let a flip-model DXGI Present sleep while RendererLock is held: during
    // resize/compositor transitions that would block the source thread from
    // producing new completed frames. A busy swap chain is retried by the
    // presentation thread without losing the queued frame.
    const bool nonBlockingCompleted = completed &&
        (m_UsedSwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
         m_UsedSwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    const UINT presentFlags = nonBlockingCompleted ? DXGI_PRESENT_DO_NOT_WAIT : 0;
    g_bPresent=true;
    hr = m_pDXGISwapChain1->Present(1u, presentFlags);
    g_bPresent=false;
    if (hr == S_OK) m_completedPresentPermit = false;
    if (trace) {
        const auto finished = GetPreciseTick();
        mfg::SourceCadenceTrace().Record("completed_swap", finished, timestamp, -1,
            serial, 0, 0, 0, 0, true, m_mfgLostGenerated, hr, finished - presentStarted);
    }
    if(SUCCEEDED(hr)) m_pFilter->OnVideoPresented();
    return hr;
}

HRESULT CDX11VideoProcessor::CompositeMfg(const mfg::FrameSurface& surface,REFERENCE_TIME timestamp,bool nativeInput,uint64_t serial,bool original) {
    CComPtr<ID3D11Texture2D> back;
    HRESULT hr=m_pDXGISwapChain1->GetBuffer(0,IID_PPV_ARGS(&back));
    if(FAILED(hr))return hr;
    const bool projected = CanProjectJvr();
    hr=RenderMfgVideo(surface, nativeInput, back, timestamp, PictureForPlayback(projected), m_mfgLastVideoOnlyInput, false, projected);
    if(FAILED(hr))return hr;
    return FinishMfgPresent(back,timestamp,serial,false,nullptr,false,false,original);
}

int CDX11VideoProcessor::GetRenderAhead() {
    if(m_mfgMultiplier&&!m_mfg.Failed()&&!m_mfgPresentFailed&&!MfgSourceMeetsDisplayRate()) {
        // Optional A/B switch for the previous one-frame render-ahead policy.
        static const bool extendedAhead = [] {
            wchar_t setting[8]{};
            return GetEnvironmentVariableW(L"MPCVR_MFG_RENDER_AHEAD",setting,_countof(setting)) == 0 || setting[0] != L'0';
        }();
        const REFERENCE_TIME duration = m_pFilter->m_FrameStats.GetAverageFrameDuration();
        // Keep the existing 1.5-frame experimental lookahead, but now three
        // banks can retain the earlier pictures while MFG gets ahead.
        // Disable extended lookahead with MPCVR_MFG_RENDER_AHEAD=0.
        REFERENCE_TIME ahead = extendedAhead ? duration * 3 / 2 + 100000 : duration + 100000;
        if (extendedAhead) {
            const auto pendingRepeats = m_mfgPendingRepeats.load(std::memory_order_relaxed);
            if (pendingRepeats) {
                // Read ahead across the confirmed repeats without moving any display deadline.
                const auto phaseMargin = std::clamp<REFERENCE_TIME>(duration / 2, 0, 200000);
                // The first interpolated frame is due before the other outputs
                // from the same NGX submission. Give the batch one display slot
                // of extra headroom only while crossing a confirmed repeat.
                const auto displaySlot = REFERENCE_TIME(std::clamp(m_dRefreshPeriodMs, 0.0, 25.0) * 10000.0);
                ahead = std::max(ahead, duration * (std::min(pendingRepeats, mfg::MaxGenerated) + 1)
                    + 100000 + phaseMargin + displaySlot);
            }
        }
        const REFERENCE_TIME adaptive = extendedAhead && AdaptiveAheadEnabled()
            ? m_mfgExtraAheadTicks.load(std::memory_order_relaxed) : 0;
        return int(std::clamp<REFERENCE_TIME>(ahead+adaptive,100000,2500000));
    }
    return 0;
}
