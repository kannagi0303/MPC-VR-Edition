/*
* (C) 2018-2026 see Authors.txt
*
* This file is part of MPC-BE.
*
* MPC-BE is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 3 of the License, or
* (at your option) any later version.
*
* MPC-BE is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#include "stdafx.h"
#include "Helper.h"
#include "Times.h"
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"
#include "Mfg/CadenceTrace.h"
#include "Edition/GpuLoadSampler.h"
#include "Edition/CompletedBufferAudit.h"
#include <cmath>
#include <utility>

void CDX11VideoProcessor::RecordEditionVsr(const CRect& input, const CRect& output, bool afterMfg)
{
	if (m_VendorId == PCIV_NVIDIA && m_bVPUseSuperRes &&
		output.Width() > input.Width() && output.Height() > input.Height()) {
		m_editionVsr = {input.Size(), output.Size(), GetTickCount64(), afterMfg};
	}
}

std::optional<size_t> CDX11VideoProcessor::AppendEditionStatistics(std::wstring& str, double /*sourceFps*/)
{
	const bool showInformation = m_showFeatureStatus.load(std::memory_order_relaxed);
	auto& bufferTrace = mfg::SourceCadenceTrace();
	if (!showInformation && !bufferTrace.Enabled()) return std::nullopt;
	const bool showDetails = showInformation && m_showEditionDetails;
	const bool running = m_pFilter->m_filterState == State_Running;
	const auto now = GetTickCount64();
	const auto run = m_pFilter->m_featureStatusPlaybackRun.load(std::memory_order_relaxed);
	if (m_editionStatisticsRun != run) {
		m_editionStatisticsRun = run;
		m_completedBufferStatsTick = 0;
		m_completedBufferPercent.reset();
		m_completedBufferLow = {};
		m_completedBufferStats.clear();
        m_visualTimeline = {};
		std::lock_guard guard(m_mfgRateLock);
		m_mfgRateTick = 0;
		m_mfgDxgiHasBaseline = false;
		m_mfgDxgiAvailable = false;
		m_mfgDxgiStatus = L"baseline";
	}

	const wchar_t* mfgReason = nullptr;
	double outputFps = 0.0, skippedPerSec = 0.0;
	std::optional<double> displayFps;
	const wchar_t* dxgiStatus = L"inactive";
	if (m_mfgMultiplier) {
		if (MfgSourceMeetsDisplayRate()) {
			mfgReason = L"Source meets display refresh";
		} else if (m_mfgPresentFailed) {
			mfgReason = L"Presenter failed";
		} else if (m_mfg.Failed()) {
			mfgReason = L"Unavailable";
		} else if (!running) {
			// Leave rate sampling stopped while paused.
		} else {
			const auto rateTick=int64_t(GetPreciseTick());
			const auto real=m_mfgRealPresents.load(std::memory_order_relaxed);
			const auto generated=m_mfgGeneratedPresents.load(std::memory_order_relaxed);
			const auto fallback=m_mfgFallbackPresents.load(std::memory_order_relaxed);
			const auto skipped=m_mfgSkipped.load(std::memory_order_relaxed);
			{
				std::lock_guard guard(m_mfgRateLock);
				const double frequency=double(GetPreciseTicksPerSecond());
				if (!m_mfgRateTick || rateTick<m_mfgRateTick) {
					m_mfgRateTick=rateTick;
					m_mfgDxgiStatus = L"baseline";
					if (mfg::SourceCadenceTrace().Enabled()) {
						mfg::SourceCadenceTrace().Record("mfg_dxgi_status_rate_reset", rateTick, -1, -1,
							0, 0, 0, 0, 0, false, m_mfgLostGenerated, S_OK);
					}
					m_mfgRateReal=real;
					m_mfgRateGenerated=generated;
					m_mfgRateFallback=fallback;
					m_mfgRateSkipped=skipped;
					m_mfgActualFps=m_mfgSkippedPerSec=m_mfgFallbackFps=0.0;
				}
				else if (frequency>0 && double(rateTick-m_mfgRateTick)>=frequency) {
					const double seconds=double(rateTick-m_mfgRateTick)/frequency;
					const auto realDelta=real>=m_mfgRateReal?real-m_mfgRateReal:0;
					const auto genDelta=generated>=m_mfgRateGenerated?generated-m_mfgRateGenerated:0;
					const auto fallbackDelta=fallback>=m_mfgRateFallback?fallback-m_mfgRateFallback:0;
					const auto skipDelta=skipped>=m_mfgRateSkipped?skipped-m_mfgRateSkipped:0;
					m_mfgActualFps=double(realDelta+genDelta+fallbackDelta)/seconds;
					if (mfg::SourceCadenceTrace().Enabled()) {
						// Window counts are stored in serial (source), phase (generated),
						// and effective_multiplier (fallback). The unit of duration is QPC.
						mfg::SourceCadenceTrace().Record("mfg_stats_window", rateTick, -1, -1,
							realDelta, unsigned(genDelta), unsigned(fallbackDelta), 0, 0, false,
							m_mfgLostGenerated, S_OK, rateTick - m_mfgRateTick);
					}
					m_mfgFallbackFps=double(fallbackDelta)/seconds;
					m_mfgSkippedPerSec=double(skipDelta)/seconds;
					// Read DXGI's last display statistics once per rate window, never in
					// the critical Present path. Unsupported/disjoint windows show n/a.
					DXGI_FRAME_STATISTICS dxgi{};
					const bool supported=m_pDXGISwapChain1 &&
						(m_bExclusiveScreen || m_UsedSwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD ||
						 m_UsedSwapEffect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
					const HRESULT statsHr=supported?m_pDXGISwapChain1->GetFrameStatistics(&dxgi):E_FAIL;
					// [MFG-B10 | 2026-11-24 23:56 +08:00] Record WHY the advisory
					// DXGI rate is invalid, including failures, without changing its calculation.
					const auto previousCount = m_mfgDxgiLastPresentCount;
					const bool hadBaseline = m_mfgDxgiHasBaseline;
					const char* dxgiEvent = "mfg_dxgi_status_unsupported";
					m_mfgDxgiStatus = L"unsupported";
					if(SUCCEEDED(statsHr)) {
						if (mfg::SourceCadenceTrace().Enabled()) {
							mfg::SourceCadenceTrace().Record("mfg_dxgi_sample", rateTick, -1, -1,
								dxgi.PresentCount, dxgi.PresentRefreshCount, dxgi.SyncRefreshCount, 0,
								dxgi.SyncQPCTime.QuadPart, false, m_mfgLostGenerated, statsHr);
						}
						if(m_mfgDxgiHasBaseline && dxgi.PresentCount>=m_mfgDxgiLastPresentCount) {
							const double rate=double(dxgi.PresentCount-m_mfgDxgiLastPresentCount)/seconds;
							const double displayHz=m_dRefreshPeriodMs>0.0?1000.0/m_dRefreshPeriodMs:0.0;
							// DXGI stats can be discontinuous on mode changes / some display paths.
							// Never report an impossible scan-out rate as a measurement.
							m_mfgDxgiAvailable=displayHz>0.0 && rate<=displayHz*1.2+1.0;
							if(m_mfgDxgiAvailable) {
								m_mfgDxgiDisplayFps=rate;
								dxgiEvent = "mfg_dxgi_status_valid";
								m_mfgDxgiStatus = L"valid";
							} else if (displayHz <= 0.0) {
								dxgiEvent = "mfg_dxgi_status_refresh_unknown";
								m_mfgDxgiStatus = L"refresh unknown";
							} else {
								dxgiEvent = "mfg_dxgi_status_rate_rejected";
								m_mfgDxgiStatus = L"rate rejected";
							}
						} else {
							m_mfgDxgiAvailable=false;
							dxgiEvent = hadBaseline ? "mfg_dxgi_status_count_reversed" : "mfg_dxgi_status_baseline";
							m_mfgDxgiStatus = hadBaseline ? L"count reversed" : L"baseline";
						}
						m_mfgDxgiLastPresentCount=dxgi.PresentCount;
						m_mfgDxgiHasBaseline=true;
					} else {
						m_mfgDxgiHasBaseline=false;m_mfgDxgiAvailable=false;
						if (supported) {
							const bool disjoint = statsHr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
							dxgiEvent = disjoint ? "mfg_dxgi_status_disjoint" : "mfg_dxgi_status_api_failed";
							m_mfgDxgiStatus = disjoint ? L"disjoint" : L"API failed";
						}
					}
					if (mfg::SourceCadenceTrace().Enabled()) {
						mfg::SourceCadenceTrace().Record(dxgiEvent, rateTick, -1, -1,
							dxgi.PresentCount, previousCount, dxgi.PresentRefreshCount, dxgi.SyncRefreshCount,
							dxgi.SyncQPCTime.QuadPart, hadBaseline, m_mfgLostGenerated, statsHr, rateTick - m_mfgRateTick);
					}
					m_mfgRateTick=rateTick;
					m_mfgRateReal=real;
					m_mfgRateGenerated=generated;
					m_mfgRateFallback=fallback;
					m_mfgRateSkipped=skipped;
				}
				outputFps=m_mfgActualFps;
				skippedPerSec=m_mfgSkippedPerSec;
				if (m_mfgDxgiAvailable) displayFps = m_mfgDxgiDisplayFps;
				dxgiStatus = m_mfgDxgiStatus;
			}
		}
	}

	const auto sr = m_dlssSR.GetStats();
	const bool srSelected = m_iVPSuperRes == SUPERRES_DLSS_SR;
	const bool srActive = srSelected && !m_dlssSRInputStatus && m_dlssSR.Active() && sr.inputWidth && sr.outputWidth;
	const bool vsrActive = !srSelected && m_bVPUseSuperRes && m_editionVsr.tick &&
		(!running || now - m_editionVsr.tick <= 1000);
	

	const bool depthEnabled = UsesDepthSr() || UsesDepthMfg();
	const auto& depth = m_depth.GetCounters();
	
	
    const bool svdOff = !m_pFilter->m_videoEnhancement.load();
    const bool svdRaw = !svdOff;
    const bool svdBUnsupported = svdRaw && (SourceIsHDR() || m_bVPUseRTXVideoHDR ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || m_pPSHalfOUtoInterlace);
    const auto svdStats = svdOff ? L"Off" : svdBUnsupported ? L"SDR required" :
        (m_svdLutRawTest.failed || m_svdLutRawTest.model.GetState() == svdlut::ModelWorker::State::Failed) ? L"Fallback" :
        m_svdLutRawTest.active ? L"Active" : L"Preparing";

	const bool hasVideoBuffer = m_completedLast || CompletedFramesPending();
	const bool refreshBufferStats = hasVideoBuffer && running &&
		(!m_completedBufferStatsTick || now - m_completedBufferStatsTick >= edition::BufferLowWatermark::SampleIntervalMs ||
        (showInformation && m_cachedStatisticsEpoch != m_pFilter->m_completedEpoch));
	CRefTime bufferStream;
	const bool needDepthPts = showDetails && running && (depthEnabled || !svdOff);
    if (!hasVideoBuffer || m_cachedStatisticsEpoch != m_pFilter->m_completedEpoch) m_visualTimeline = {};
	const bool hasStreamTime = (refreshBufferStats || needDepthPts) && SUCCEEDED(m_pFilter->StreamTime(bufferStream));
	const int64_t playbackPts = needDepthPts && hasStreamTime ? REFERENCE_TIME(bufferStream) - m_rtVideoDelay : -1;

	if (refreshBufferStats && hasStreamTime) {
		const auto time = GetCompletedBufferTime(bufferStream);
		const auto target = m_extendedStartupPending ? CompletedStartupLead : CompletedLead;
		m_completedBufferPercent = edition::BufferPercent(time.ready, target);
		m_completedBufferLow.Add(now, *m_completedBufferPercent);
        if (showInformation) {
            auto& t = m_visualTimeline;
            t.BeginSnapshot();
            t.limit = CompletedLead; // fixed 2 s display axis, not an admission target
            t.sampled = time.fence != UINT64_MAX;
            t.ready = time.ready;
            t.queued = time.queued;
            const auto destination = ProcessingRect();
            const bool srInput = srSelected && !m_dlssSRAfterMfg &&
                CanDlssSRUpscale(m_srcRect, destination, false) && EarlyOriginalRoute(m_srcRect, destination);
            t.visible = {true, depthEnabled, !svdOff && !svdBUnsupported, srInput};
            const int64_t head = REFERENCE_TIME(bufferStream) - m_rtVideoDelay;
            RefreshEditionTimelineResources(t, head, time.fence);
            if (!m_visualPresentClock) t.UpdateVisual(head, m_rtAvgTimePerFrame);
        }
		m_completedBufferStats.clear();
        if (showDetails && time.nonIncreasing)
            m_completedBufferStats = std::format(L"\nBuffer order  {} out of order", time.nonIncreasing);
		// [MFG-B10 | 2026-11-24 23:56 +08:00] Independent diagnostic upper bound.
		// Same queue and fence snapshot; no policy reads this result. Signaled
		// frames after a gap are not necessarily safe continuous playback.
		if (showDetails || bufferTrace.Enabled()) {
			const auto audit = edition::AuditCompletedBuffer(m_completedFrames, time.fence, time.readyFrames);
			using Cutoff = edition::CompletedBufferAudit::Cutoff;
			const wchar_t* cutoff = L"none";
			const char* cutoffEvent = "buffer_cutoff_none";
			switch (audit.cutoff) {
			case Cutoff::Order: cutoff = L"order"; cutoffEvent = "buffer_cutoff_order"; break;
			case Cutoff::Fence: cutoff = L"fence"; cutoffEvent = "buffer_cutoff_fence"; break;
			case Cutoff::OrderAndFence: cutoff = L"order+fence"; cutoffEvent = "buffer_cutoff_both"; break;
			case Cutoff::NoFence: cutoff = L"no valid fence"; cutoffEvent = "buffer_cutoff_no_fence"; break;
			default: break;
			}
			const auto signaledAhead = audit.signaledFrames ? (std::max)(REFERENCE_TIME(0), audit.signaledEnd - REFERENCE_TIME(bufferStream)) : 0;
			if (showDetails) {
				m_completedBufferStats += std::format(L"\nBuffer audit  Signaled {} ms ({}) | Cut {} | Batch {}",
					signaledAhead / 10000, audit.signaledFrames, cutoff, m_completedBatches.size());
			}
			if (bufferTrace.Enabled()) {
				const auto tick = GetPreciseTick();
				const mfg::CadenceTrace::CompletedQueue queue{m_completedFrames.size(), m_completedBatches.size(),
					audit.queuedEnd, m_completedDisplayPacing.LeadMs(), m_completedDisplayPacing.LeadMs(), m_completedDisplayPacing.Samples()};
				bufferTrace.Record("buffer_audit", tick, audit.readyEnd, bufferStream,
					time.fence, unsigned(time.readyFrames), unsigned(time.queuedFrames), unsigned(audit.signaledFrames),
					audit.queuedEnd, CompletedGeometryChanging(), m_mfgLostGenerated, S_OK, 0, audit.signaledEnd, &queue);
				bufferTrace.Record(cutoffEvent, tick, audit.priorDue, bufferStream, audit.cutoffFence,
					unsigned(audit.cutoffIndex), time.nonIncreasing, *m_completedBufferPercent, audit.cutoffDue,
					displayFps.has_value(), m_mfgLostGenerated, S_OK, 0, target);
			}
		}
        if (showDetails) {
            const auto& d = m_completedStatsDrops;
            if (d.ngxLate || d.composeLate || d.presentLate || d.budget || d.other)
                m_completedBufferStats += std::format(L"\nMFG skip window: NGX {} compose {} present {} budget {} other {}",
                    d.ngxLate, d.composeLate, d.presentLate, d.budget, d.other);
        }
		m_completedStatsDrops = {};
		m_completedBufferStatsTick = now;
	}

	if (!showInformation) return std::nullopt;
    if (refreshBufferStats && !hasStreamTime) m_visualTimeline = {};
    m_visualGeometryDirty = true;

    const auto editionStart = str.size();
    // A fixed 61-character logical grid. Long diagnostic strings belong to the
    // optional existing detailed view, not the compact rows.
    const auto fit = [](std::wstring value, size_t width, bool right = false) { return edition::visual::Fit(std::move(value), width, right); };
    const auto pair = [&](std::wstring a, std::wstring av, std::wstring b, std::wstring bv) {
        const auto row = edition::visual::PairRow(a, av, b, bv);
        if (!row.empty()) str += L"\n" + row;
    };
    const auto divider = [&] { str += L"\n" + std::wstring(edition::visual::Timeline::Columns, L'-'); };
    auto& t = m_visualTimeline;
    t.textRow = -1;
    if (!depthEnabled) { t.visible[1] = false; t.cutCount = 0; }
    if (svdOff || svdBUnsupported) t.visible[2] = false;
    if (!srSelected || m_dlssSRAfterMfg) t.visible[3] = false;
    str += L"\n" + fit(std::format(L"Sync {:+} ms", (m_RenderStats.syncoffset + 5000) / 10000), 31);
    if (displayFps && running) str += fit(std::format(L"Present {:.2f} fps", *displayFps), 30, true);
    else if (outputFps > 0 && running) str += fit(std::format(L"MFG submit {:.2f} fps", outputFps), 30, true);
    else if (!running) str += fit(L"Paused / stopped", 30, true);
    else {
        const double drawFps = m_pFilter->m_DrawStats.GetAverageFps();
        str += fit(drawFps > 0.0 && std::isfinite(drawFps) ? std::format(L"Draw {:.2f} fps", drawFps) : L"Draw n/a", 30, true);
    }
    const bool haveMfgRate = running && m_mfgMultiplier && !mfgReason && outputFps > 0.0;
    const auto skipText = haveMfgRate ? std::format(L"MFG Skip {:.1f}/s", skippedPerSec) : std::wstring(L"MFG Skip n/a");
    if (hasVideoBuffer && t.sampled) {
        str += L"\n" + fit(running ? L"Video Buffer" : L"Buffer (last)", 25) +
            fit(std::format(L"{:.2f} / {:.2f} s", double(t.ready)/UNITS, double(t.limit)/UNITS), 36, true);
        // The timeline begins on the next text row. Account for the existing
        // separator row inserted at editionStart. Blank label rows stay fixed.
        str += L"\n" + edition::visual::Axis();
        t.textRow = edition::visual::FirstTrackRow(str);
        const wchar_t* labels[] = {L"Video", L"Depth", L"LUT", L"SR Input"};
        for (size_t i=0; i<t.visible.size(); ++i)
            str += L"\n" + fit(t.visible[i] ? labels[i] : L"", edition::visual::Timeline::Columns);
        const auto low = m_completedBufferLow.Minimum(now);
        const auto lost = m_mfgLostReal.load(std::memory_order_relaxed);
        const auto dropped = lost >= m_editionOriginalDropsBase ? lost - m_editionOriginalDropsBase : 0;
        str += L"\n" + fit(low ? std::format(L"10s Min {}%", *low) : L"10s Min n/a", 20) +
            fit(m_mfgMultiplier ? std::format(L"MFG Orig Lost {}", dropped) : L"", 21) +
            fit(m_mfgMultiplier ? skipText : L"", 20, true);
    } else if (hasVideoBuffer) {
        str += L"\nVideo Buffer: preparing / unavailable";
    }

    edition::GpuLoadSnapshot gpu;
    if (m_VendorId == PCIV_NVIDIA) {
        if (!m_gpuLoadSampler) m_gpuLoadSampler = std::make_unique<edition::GpuLoadSampler>();
        gpu = m_gpuLoadSampler->Read(); // existing observer and 900 ms sampler, once per stats refresh
        const auto load = [](int value) { return value >= 0 ? std::format(L"{}%", value) : std::wstring(L"n/a"); };
        divider();
        using edition::visual::Field;
        str += L"\n" + edition::visual::HardwareRow({load(gpu.gpu), load(gpu.memory), load(gpu.decoder), load(gpu.opticalFlow)});
        const auto freeValue = gpu.gpu >= 0 && std::isfinite(m_dRefreshPeriodMs) && m_dRefreshPeriodMs > 0.0
            ? std::format(L"{:.2f} / {:.2f} ms", m_dRefreshPeriodMs * (100-gpu.gpu)/100.0, m_dRefreshPeriodMs)
            : std::wstring(L"n/a");
        str += L"\n" + Field(L"Free (est.)", freeValue, 29, 12);
    }
    if (depthEnabled || srSelected || vsrActive || m_mfgMultiplier) {
        divider();
        std::wstring depthValue, srValue, mfgValue;
        if (depthEnabled) {
            if (m_depth.Loading()) depthValue = L"Loading";
            else if (m_depth.Unavailable()) depthValue = L"Unavailable";
            else {
                if (depth.mapRateSampleTick && (!running || (now >= depth.mapRateSampleTick && now-depth.mapRateSampleTick <= 2000)))
                    depthValue = std::format(L"{:.1f}/s", depth.mapsPerSecond);
                if (depth.gpuTimings.available) {
                    if (!depthValue.empty()) depthValue += L" ";
                    depthValue += std::format(L"{:.2f} ms", depth.gpuTimings.packMs + depth.gpuTimings.modelMs + depth.gpuTimings.postMs);
                }
                if (depthValue.empty()) depthValue = L"n/a";
            }
        }
        if (srSelected) {
            if (srActive) srValue = sr.ngxGpuTimeValid ? std::format(L"{:.2f} ms", sr.ngxGpuMs) : L"n/a";
            else if (m_dlssSRInputStatus && std::wstring_view(m_dlssSRInputStatus) == L"source exceeds DLSS size threshold")
                srValue = L"Size limit";
            else if (m_dlssSR.Preparing()) srValue = L"Initializing";
            else srValue = L"Bypassed";
        }
        if (m_mfgMultiplier) {
            if (MfgSourceMeetsDisplayRate()) mfgValue = L"Source rate";
            else if (m_mfgPresentFailed) mfgValue = L"Present failed";
            else if (mfgReason) mfgValue = L"Unavailable";
            else if (!running) mfgValue = L"Paused";
            else if (!m_mfg.InputSize().cx) mfgValue = L"Starting";
            else {
                const auto effective = m_mfgEffectiveMultiplier.load(std::memory_order_relaxed);
                const bool automatic = m_mfgMultiplier == mfg::AutoMultiplier;
                mfgValue = effective ? std::format(L"{}x{}", automatic ? L"Auto " : L"", effective) : L"Starting";
            }
        }
        pair(depthEnabled ? L"Depth" : L"", depthValue, srSelected ? L"DLSS SR" : vsrActive ? L"RTX VSR" : L"", srValue);
        pair(m_mfgMultiplier ? L"DLSS MFG" : L"", mfgValue, L"", L"");
        if (m_mfgPresentFailed || (m_mfgMultiplier && m_mfg.Failed())) str += L"\nMFG fallback / failure";
    }
    if (srSelected && !srActive && m_dlssSRInputStatus &&
        std::wstring_view(m_dlssSRInputStatus) != L"source exceeds DLSS size threshold")
        str += L"\n" + fit(std::format(L"DLSS SR: {}", m_dlssSRInputStatus), 61);
    // These are effective configured strengths, not inferred per-frame quality or
    // success percentages. The master switch already zeroes disabled controls.
    const std::array<int, 4> strengths = {m_pFilter->m_videoEnhancement.load(), m_pFilter->m_memoryColor.load(),
        m_pFilter->m_adaptiveTone.load(), m_pFilter->m_skinTone.load()};
    const bool sdrEffectsSupported = !SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_pPSHalfOUtoInterlace &&
        m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const bool haveEffects = std::any_of(strengths.begin(), strengths.end(), [](int v) { return v > 0; });
    const bool projected = CanProjectJvr();
    if (haveEffects || projected) {
        divider();
        if (haveEffects && sdrEffectsSupported) {
            str += edition::visual::EnhancementRows(strengths);
            if (!svdOff && std::wstring_view(svdStats) != L"Active")
                str += L"\n" + fit(std::format(L"Color Enhancement: {}", svdStats), 61);
            if (strengths[2] > 0 && m_adaptiveTone.shaderFailed) str += L"\nAdaptive Tone: shader unavailable";
            if (PerceptualEnabled() && (m_perceptual.failedStandalone || m_perceptual.failedB))
                str += L"\nColor processing: shader fallback";
        } else if (haveEffects) str += L"\nColor processing: SDR required";
        if (projected) str += L"\nVR Projection";
    }
    if (m_mfgMultiplier && !(hasVideoBuffer && t.sampled) && running) {
        const auto lost = m_mfgLostReal.load(std::memory_order_relaxed);
        const auto dropped = lost >= m_editionOriginalDropsBase ? lost-m_editionOriginalDropsBase : 0;
        pair(L"MFG Orig Lost", std::to_wstring(dropped), L"MFG Skip", haveMfgRate ? std::format(L"{:.1f}/s", skippedPerSec) : L"n/a");
    }

    if (showDetails) {
        divider();
        str += L"\nDebug";
        if (const auto corrected = m_pFilter->m_correctedSourcePts)
            str += std::format(L"\nSource PTS    {} corrected", corrected);
        if (const auto backwards = m_pFilter->m_FrameStats.GetBackwardsTimestamps())
            str += std::format(L"\nSource PTS    {} out of order", backwards);
        if (!displayFps && running && m_mfgMultiplier && !mfgReason)
            str += std::format(L"\nDXGI          n/a ({})", dxgiStatus);
        if (hasVideoBuffer) str.append(m_completedBufferStats);
        if (m_completedResizeEarlyDrops)
            str += std::format(L"\nMFG resize    {} early drops", m_completedResizeEarlyDrops);
        if (depthEnabled) {
            if (UsesDepthMfg() && depth.rawMfgCheckAge >= 0)
                str += std::format(L"\nDepth check   {} | {:.1f} ms age", depth.rawMfgCheck, double(depth.rawMfgCheckAge) / 10000.0);
            const auto cache = m_depth.GetCacheStats(playbackPts);
            if (cache.capacity)
                str += std::format(L"\nDepth cache   {}/{} | {} late maps", cache.cached, cache.capacity, depth.lateMaps);
            else if (m_depth.Unavailable())
                str += std::format(L"\nDepth reason  {}", m_depth.Status());
        }
        if (PerceptualEnabled() && (m_perceptual.failedStandalone || m_perceptual.failedB))
            str += L"\nPerceptual    shader compile failed (fallback)";
        if (depthEnabled) {
            if (UsesDepthSr() && depth.srDepthRequests)
                str += std::format(L"\nDepth SR      {}/{} evaluations", depth.srEvaluations, depth.srDepthRequests);
            if (UsesDepthMfg() && depth.mfgDepthRequests)
                str += std::format(L"\nDepth MFG     {}/{} submissions", depth.mfgSubmissions, depth.mfgDepthRequests);
        }
        if (svdRaw) {
            const auto& b = m_svdLutRawTest;
            if (b.sourceLutChecks)
                str += std::format(L"\nLUT exact     {}/{} checks", b.sourceLutExactReady, b.sourceLutChecks);
            if (b.sourceLutDraws)
                str += std::format(L"\nLUT future    {}/{} draws", b.sourceLutFutureDraws, b.sourceLutDraws);
            if (b.skippedCapture || b.b2BudgetSkips)
                str += std::format(L"\nVE optional skips: capture {} | luma {}", b.skippedCapture, b.b2BudgetSkips);
            if (b.sourceLutDraws)
                str += std::format(L"\nVE future contribution {:.1f}% mean", 100.0 * b.sourceLutFutureShareSum / b.sourceLutDraws);
            if (b.b2MemoryShaderFailed) str += L"\nVE chroma     shader compile failed";
            if (b.b2ShaderFailed) str += L"\nVE luma       shader compile failed";
            if (b.model.GetState() == svdlut::ModelWorker::State::Failed || b.failed)
                str += std::format(L"\nVE model error {}", SvdLutStatus());
        }
    }

	if (str.size() == editionStart) return std::nullopt;
	str.insert(editionStart, 1, L'\n');
	return editionStart + 1;
}

void CDX11VideoProcessor::RefreshEditionTimelineResources(edition::visual::Timeline& t, int64_t head, uint64_t fence)
{
    // Existing CPU metadata only; no new clock/fence queries, readback, or polling.
    for (auto& row : t.rows) row.count = 0;
    t.cutCount = 0;
    if (!t.sampled || head < 0 || fence == UINT64_MAX) return;
    const int64_t duration = m_rtAvgTimePerFrame;
    if (t.visible[1] && (UsesDepthSr() || UsesDepthMfg())) {
        static_assert(CDepthGpu::Slots <= edition::visual::Ranges::MaxSamples);
        for (auto pts : m_depth.CachedResultPts()) t.AddSample(1, pts, head, duration);
        for (auto pts : m_depth.SceneCutPts()) t.AddCut(pts, head);
    }
    if (t.visible[2] && m_pFilter->m_videoEnhancement.load()) {
        const auto& b = m_svdLutRawTest;
        for (const auto& f : b.frames)
            if (f.view && f.parameters && (!f.fence || f.fence <= fence))
                t.AddSample(2, f.pts, head, duration);
        if (b.heldView && b.heldParameters && (!b.heldFence || b.heldFence <= fence))
            t.AddSample(2, b.heldPts, head, duration);
    }
    if (t.visible[3] && m_iVPSuperRes == SUPERRES_DLSS_SR && !m_dlssSRAfterMfg) {
        const auto add = [&](const EarlyOriginalImage& image) {
            // Retained RGB identity, not proof of DLSS SR completion.
            if (image.ready && image.epoch == m_pFilter->m_completedEpoch &&
                image.srSource && !image.consumed && image.rgb.pTexture && EarlyOriginalDomainMatches(image))
                t.AddSample(3, image.pts, head, duration);
        };
        add(m_earlyOriginal); add(m_futureOriginal);
    }
    for (auto& row : t.rows) row.Merge();
}

void CDX11VideoProcessor::DrawEditionTimeline(const SIZE& rtSize)
{
    auto& t = m_visualTimeline;
    if (!t.sampled || t.textRow < 0 || t.limit <= 0 || rtSize.cx <= 0 || rtSize.cy <= 0) return;
    const bool layoutDirty = m_visualGeometryDirty || rtSize.cx != m_visualGeometrySize.cx || rtSize.cy != m_visualGeometrySize.cy;
    if (layoutDirty) {
        m_visualGeometryDirty = false;
        m_visualGeometryValid = false;
        m_visualGeometrySize = rtSize;
        m_visualLine = 0;
        SIZE label{}, full{}, lines{};
        constexpr wchar_t labelSpaces[] = L"0000000000";
        constexpr wchar_t fullSpaces[] = L"0000000000000000000000000000000000000000000000000000000000000";
        static_assert(std::size(labelSpaces)-1 == edition::visual::Timeline::LabelColumns);
        static_assert(std::size(fullSpaces)-1 == edition::visual::Timeline::Columns);
        if (FAILED(m_Font3D.GetTextExtent(labelSpaces, &label)) || FAILED(m_Font3D.GetTextExtent(fullSpaces, &full)) ||
            FAILED(m_Font3D.GetTextExtent(L"0\n0\n0\n0\n0\n0\n0\n0", &lines))) return;
        const LONG line = (lines.cy + 4) / 8;
        if (line < 3 || label.cx <= 0 || full.cx <= label.cx) return;
        const LONG left = m_StatsTextPoint.x + label.cx;
        const LONG right = m_StatsTextPoint.x + full.cx;
        const LONG top = m_StatsTextPoint.y + m_statsFeatureOffset + t.textRow * line;
        if (left < 0 || top < 0 || right > rtSize.cx || top + edition::visual::Timeline::Rows * line > rtSize.cy) return;
        m_visualLayout.SetRect(left, top, right, top + edition::visual::Timeline::Rows * line);
        m_visualLine = line;
    }
    if (!m_visualLine) return;
    // On the scheduled Completed path, project the current retained samples
    // using the exact stream clock/fence already read by that Present. Never
    // scroll an old snapshot and then jump it to the latest producer frontier.
    // Other paths retain their last truthful snapshot rather than invent motion.
    if (m_visualPresentClock && m_pFilter->m_filterState == State_Running) {
        const auto head = m_visualPresentClock->stream - m_rtVideoDelay;
        RefreshEditionTimelineResources(t, head, m_visualPresentClock->fence);
        t.UpdateVisual(head, m_rtAvgTimePerFrame);
    }
    m_visualRaster.clear();
    edition::visual::Emit(t, m_visualLayout.left, m_visualLayout.top, m_visualLayout.right, m_visualLine,
        [&](int l,int y,int r,int b,uint32_t color) { m_visualRaster.push_back({l,y,r,b,color}); });
    if (m_visualGeometryValid && m_visualRaster.size() == m_visualUploadedRaster.size()) {
        for (size_t i = 0; i < m_visualRaster.size(); ++i)
            edition::visual::HoldPixelJitter(m_visualRaster[i], m_visualUploadedRaster[i]);
    }
    if (m_visualGeometryValid && m_visualRaster == m_visualUploadedRaster) {
        m_visualGeometry.Draw();
        return;
    }
    if (!m_visualGeometry.Initialized()) {
        if (FAILED(m_visualGeometry.InitDeviceObjects(m_pDevice, m_pDeviceContext)) || !m_visualGeometry.Initialized()) {
            m_visualGeometry.InvalidateDeviceObjects();
            m_visualLine = 0; // retry only on the next snapshot/layout update
            return;
        }
    }
    SIZE size = rtSize;
    m_visualGeometry.ClearPoints(size);
    for (const auto& r : m_visualRaster)
        m_visualGeometry.Rect(r.left,r.top,r.right,r.bottom,static_cast<D3DCOLOR>(r.color));
    m_visualGeometryValid = m_visualGeometry.UpdateVertexBuffer() == S_OK;
    if (m_visualGeometryValid) {
        m_visualRaster.swap(m_visualUploadedRaster);
        m_visualGeometry.Draw();
    } else {
        m_visualLine = 0; // do not repeatedly retry a failed upload in Present
    }
}

void CDX11VideoProcessor::InsertEditionStatsSeparator(std::wstring& str, size_t separator)
{
	SIZE dash{};
	m_Font3D.GetTextExtent(L"-", &dash);
	const LONG right = std::min<LONG>(m_StatsRect.right - 3, m_windowRect.Width());
	const LONG width = std::max<LONG>(0, right - m_StatsTextPoint.x);
	str.insert(separator, dash.cx > 0 ? width / dash.cx : 0, L'-');
}

std::wstring CDX11VideoProcessor::GetMfgStatus() {
		const unsigned multiplier = m_mfgMultiplier.load();
		if (!multiplier) return L"Processor MFG: Off (NGX is not called)";
		if (MfgSourceMeetsDisplayRate()) return L"Processor MFG: Bypassed (source meets display refresh)";
		if (m_mfgPresentFailed.load()) return L"Processor MFG: " + (multiplier==mfg::AutoMultiplier ? L"Auto" : L"x"+std::to_wstring(multiplier)) +
			L"; presenter failed, normal playback retained";
		std::lock_guard guard(m_mfgRateLock);
		std::wstring diagnostics = std::format(
			L"\nMFG runtime    : cap x{}, needs x{}; fallback {:.1f}/s, skipped {:.1f}/s",
			m_mfg.MaxAvailableMultiplier(), m_mfgTargetSlots, m_mfgFallbackFps, m_mfgSkippedPerSec);
		if (UsesDepthMfg()) {
			diagnostics += std::format(L"\nMFG depth      : {}", m_mfg.UsesDepth() ? L"depth active" :
				!MfgDepthSourceEligible() ? L"flat (unsupported source)" : m_depth.Status());
		}
		const std::wstring& ngx = m_mfg.Status();
		return L"Processor MFG: " + (multiplier==mfg::AutoMultiplier ? L"Auto" : L"x"+std::to_wstring(multiplier)) + L"; " +
			(ngx.empty() ? L"NGX not yet attempted (waiting for eligible running frame)" : ngx) +
			L"; presents generated=" + std::to_wstring(m_mfgGeneratedPresents.load()) +
			L" real=" + std::to_wstring(m_mfgRealPresents.load()) +
			L" skipped=" + std::to_wstring(m_mfgSkipped.load()) +
			L"; original protection " + mfg::OriginalFrameProtectionName(MfgOriginalProtection) +
			L", replaced=" + std::to_wstring(m_mfgOriginalsReplaced) + diagnostics + MfgFrameAnalysisStatus();
	}


void CDX11VideoProcessor::AppendEditionProcessorInfo(std::wstring& str)
{
	if (m_processingSize.cx > 0) {
		str += std::format(L"\nProcessing size : {}x{}; video destination {}x{}", m_processingSize.cx,
			m_processingSize.cy, m_videoRect.Width(), m_videoRect.Height());
	}
	if (m_iVPSuperRes == SUPERRES_DLSS_SR) {
		if (m_dlssSRInputStatus) {
			str += std::format(L"\nDLSS SR        : Inactive ({}); {}x{} to {}x{}, {}-bit, VP scaling {}",
				m_dlssSRInputStatus, m_srcRect.Width(), m_srcRect.Height(),
				m_videoRect.Width(), m_videoRect.Height(), m_srcParams.CDepth, m_bVPScaling);
		} else {
			str += std::format(L"\nDLSS SR        : {}", m_dlssSR.Status(UsesDepthSr() ? &m_depth : nullptr));
			if (m_dlssSR.Active()) {
				str += m_dlssSRAfterMfg ? L" (after MFG)" : L" (source frames)";
				if (m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) str += L" (deinterlaced)";
				const auto stats = m_dlssSR.GetStats();
				if (stats.ngxGpuTimeValid) str += std::format(L"\nDLSS SR NGX GPU: {:.2f} ms", stats.ngxGpuMs);
			}
		}
	}
	if (UsesDepthSr() || UsesDepthMfg()) {
		const auto& depth = m_depth.GetCounters();
		const auto latest = m_depth.LatestPreview();
		str += std::format(L"\nDepth          : {} captures, {} DirectML jobs, {} maps ({} late)"
			L"\nDepth usage    : {}/{} SR evaluations, {}/{} MFG submissions",
			depth.captures, depth.jobs, depth.maps, depth.lateMaps,
			depth.srEvaluations, depth.srDepthRequests, depth.mfgSubmissions, depth.mfgDepthRequests);
		if (depth.maps)
			str += std::format(L"\nDepth timing: GPU job {:.1f} ms, ready at {:.0f} ms age",
				depth.inferenceMs, double(depth.resultAge) / 10000.0);
		if (depth.gpuTimings.available)
			str += std::format(L"\nDepth GPU   : pack {:.2f} ms, model interval {:.2f} ms, post {:.2f} ms",
				depth.gpuTimings.packMs, depth.gpuTimings.modelMs, depth.gpuTimings.postMs);
		if (latest.view)
			str += std::format(L"\nDepth latest: {:.0f} ms PTS", double(latest.pts) / 10000.0);
		else
			str += std::format(L"\nDepth latest: {}", m_depth.Status());
	}
}
