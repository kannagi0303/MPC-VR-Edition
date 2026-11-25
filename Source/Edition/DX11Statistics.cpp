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

void CDX11VideoProcessor::RecordEditionVsr(const CRect& input, const CRect& output, bool afterMfg)
{
	if (m_VendorId == PCIV_NVIDIA && m_bVPUseSuperRes &&
		output.Width() > input.Width() && output.Height() > input.Height()) {
		m_editionVsr = {input.Size(), output.Size(), GetTickCount64(), afterMfg};
	}
}

std::optional<size_t> CDX11VideoProcessor::AppendEditionStatistics(std::wstring& str, double sourceFps)
{
	const bool running = m_pFilter->m_filterState == State_Running;
	const auto now = GetTickCount64();
	const auto run = m_pFilter->m_featureStatusPlaybackRun.load(std::memory_order_relaxed);
	if (m_editionStatisticsRun != run) {
		m_editionStatisticsRun = run;
		m_completedBufferStatsTick = 0;
		m_completedBufferPercent.reset();
		m_completedBufferLow = {};
		m_completedBufferStats.clear();
		std::lock_guard guard(m_mfgRateLock);
		m_mfgRateTick = 0;
		m_mfgDxgiHasBaseline = false;
		m_mfgDxgiAvailable = false;
		m_mfgDxgiStatus = L"baseline";
	}

	std::wstring mfgStats = L"Off";
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
			mfgStats = L"Paused";
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
					m_mfgRateVideoCount=m_mfgVideoCount.load(std::memory_order_relaxed);
					m_mfgRateVideoTicks=m_mfgVideoTicks.load(std::memory_order_relaxed);
					m_mfgRateGenerateCount=m_mfgGenerateCount.load(std::memory_order_relaxed);
					m_mfgRateGenerateTicks=m_mfgGenerateTicks.load(std::memory_order_relaxed);
					m_mfgRateComposeCount=m_mfgComposeCount.load(std::memory_order_relaxed);
					m_mfgRateComposeTicks=m_mfgComposeTicks.load(std::memory_order_relaxed);
					m_mfgRateLockRetries=m_mfgLockRetries.load(std::memory_order_relaxed);
					m_mfgActualFps=m_mfgGeneratedFps=m_mfgSkippedPerSec=m_mfgFallbackFps=0.0;
					m_mfgVideoWallMs=m_mfgGenerateWallMs=m_mfgComposeWallMs=0.0;
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
					m_mfgGeneratedFps=double(genDelta)/seconds;
					m_mfgFallbackFps=double(fallbackDelta)/seconds;
					m_mfgSkippedPerSec=double(skipDelta)/seconds;
					const auto videoCount=m_mfgVideoCount.load(std::memory_order_relaxed);
					const auto videoTicks=m_mfgVideoTicks.load(std::memory_order_relaxed);
					const auto videoCallCount=videoCount>=m_mfgRateVideoCount?videoCount-m_mfgRateVideoCount:0;
					const auto videoWallTicks=videoTicks>=m_mfgRateVideoTicks?videoTicks-m_mfgRateVideoTicks:0;
					m_mfgVideoWallMs=videoCallCount?1000.0*double(videoWallTicks)/frequency/double(videoCallCount):0.0;
					m_mfgRateVideoCount=videoCount;m_mfgRateVideoTicks=videoTicks;
					const auto generateCount=m_mfgGenerateCount.load(std::memory_order_relaxed);
					const auto generateTicks=m_mfgGenerateTicks.load(std::memory_order_relaxed);
					const auto composeCount=m_mfgComposeCount.load(std::memory_order_relaxed);
					const auto composeTicks=m_mfgComposeTicks.load(std::memory_order_relaxed);
					const auto workCount=generateCount>=m_mfgRateGenerateCount?generateCount-m_mfgRateGenerateCount:0;
					const auto workTicks=generateTicks>=m_mfgRateGenerateTicks?generateTicks-m_mfgRateGenerateTicks:0;
					const auto presentCount=composeCount>=m_mfgRateComposeCount?composeCount-m_mfgRateComposeCount:0;
					const auto presentTicks=composeTicks>=m_mfgRateComposeTicks?composeTicks-m_mfgRateComposeTicks:0;
					m_mfgGenerateWallMs=workCount?1000.0*double(workTicks)/frequency/double(workCount):0.0;
					m_mfgComposeWallMs=presentCount?1000.0*double(presentTicks)/frequency/double(presentCount):0.0;
					m_mfgRateGenerateCount=generateCount;m_mfgRateGenerateTicks=generateTicks;
					m_mfgRateComposeCount=composeCount;m_mfgRateComposeTicks=composeTicks;
					const auto lockRetries=m_mfgLockRetries.load(std::memory_order_relaxed);
					m_mfgLockRetriesPerSec=lockRetries>=m_mfgRateLockRetries?
						double(lockRetries-m_mfgRateLockRetries)/seconds:0.0;
					m_mfgRateLockRetries=lockRetries;
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
			if (m_mfg.InputSize().cx && sourceFps > 0.0 && outputFps > 0.0) {
				mfgStats = std::format(L"{} -> {} fps", edition::CompactNumber(sourceFps), edition::CompactNumber(outputFps));
			} else {
				mfgStats = L"Starting";
			}
		}
	}

	const auto sr = m_dlssSR.GetStats();
	const bool srSelected = m_iVPSuperRes == SUPERRES_DLSS_SR;
	const bool srActive = srSelected && m_dlssSR.Active() && sr.inputWidth && sr.outputWidth;
	const bool vsrActive = !srSelected && m_bVPUseSuperRes && m_editionVsr.tick &&
		(!running || now - m_editionVsr.tick <= 1000);
	const auto srStats = srActive ? edition::ScalingStatistics(sr.inputWidth, sr.inputHeight,
		sr.outputWidth, sr.outputHeight) : L"Off";
	const auto vsrStats = vsrActive ? edition::ScalingStatistics(m_editionVsr.input.cx, m_editionVsr.input.cy,
		m_editionVsr.output.cx, m_editionVsr.output.cy) : L"Off";

	const bool zipDepthEnabled = UsesZipDepthSr() || UsesZipDepthMfg();
	const auto& depth = m_zipDepth.GetCounters();
	const bool srDepthUsed = UsesZipDepthSr() && depth.srEvaluations && (!running || now - depth.lastSrUseTick <= 1000);
	const bool mfgDepthUsed = UsesZipDepthMfg() && depth.mfgSubmissions && (!running || now - depth.lastMfgUseTick <= 1000);
	const bool depthActive = (srDepthUsed || mfgDepthUsed) && !m_zipDepth.CoolingDown(now);
    const bool svdOff = !m_pFilter->m_videoEnhancement.load();
    const bool svdRaw = !svdOff;
    const bool svdTemporal = svdRaw;
    const bool svdBUnsupported = svdRaw && (SourceIsHDR() || m_bVPUseRTXVideoHDR ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 || m_pPSHalfOUtoInterlace);
    const auto svdStats = svdOff ? L"Off" : svdBUnsupported ? L"SDR required" :
        (m_svdLutRawTest.failed || m_svdLutRawTest.model.GetState() == svdlut::ModelWorker::State::Failed) ? L"Fallback" :
        m_svdLutRawTest.active ? L"Active" : L"Preparing";

	const bool hasVideoBuffer = m_completedLast || CompletedFramesPending();
	const bool refreshBufferStats = hasVideoBuffer && running &&
		(!m_completedBufferStatsTick || now - m_completedBufferStatsTick >= edition::BufferLowWatermark::SampleIntervalMs);
	CRefTime bufferStream;
	const bool needDepthPts = m_showEditionDetails && running && (zipDepthEnabled || !svdOff);
	const bool hasStreamTime = (refreshBufferStats || needDepthPts) && SUCCEEDED(m_pFilter->StreamTime(bufferStream));
	const int64_t playbackPts = needDepthPts && hasStreamTime ? REFERENCE_TIME(bufferStream) - m_rtVideoDelay : -1;

	if (refreshBufferStats && hasStreamTime) {
		const auto time = GetCompletedBufferTime(bufferStream);
		const auto target = m_extendedStartupPending ? CompletedStartupLead : CompletedLead;
		m_completedBufferPercent = edition::BufferPercent(time.ready, target);
		m_completedBufferLow.Add(now, *m_completedBufferPercent);
		m_completedBufferStats.clear();
		if (m_showEditionDetails) {
			m_completedBufferStats = std::format(L"\nBuffer        Ready {} s ({}) | Queued {} s ({})",
				edition::CompactNumber(double(time.ready) / UNITS), time.readyFrames,
				edition::CompactNumber(double(time.queued) / UNITS), time.queuedFrames);
			if (const auto low = m_completedBufferLow.Minimum(now)) {
				m_completedBufferStats += std::format(L" | 10 s low {}%", *low);
			}
			if (time.nonIncreasing) m_completedBufferStats += std::format(L"\nBuffer order  {} out of order", time.nonIncreasing);
		}
		// [MFG-B10 | 2026-11-24 23:56 +08:00] Independent diagnostic upper bound.
		// Same queue and fence snapshot; no policy reads this result. Signaled
		// frames after a gap are not necessarily safe continuous playback.
		auto& bufferTrace = mfg::SourceCadenceTrace();
		if (m_showEditionDetails || bufferTrace.Enabled()) {
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
			if (m_showEditionDetails) {
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
		m_completedStatsDrops = {};
		m_completedCadence.ClearWindow();
		m_completedBufferStatsTick = now;
	}

	std::wstring level = hasVideoBuffer ? L"--" : L"Off";
	if (hasVideoBuffer && m_completedBufferPercent) level = std::format(L"{}%", *m_completedBufferPercent);
	const auto separator = str.size() + 1;
	str += std::format(L"\n\nDLSS SR  {}\nVSR      {}\nMFG      {}\nZipDepth {} | Video Enhancement {} | Video buffer {}",
		srStats, vsrStats, mfgStats, depthActive ? L"On" : L"Off", svdStats, level);

	if (m_showEditionDetails) {
		// Keep the full diagnostic counters in their owners, but do not rebuild
		// a multi-page text dump every time Statistics is displayed.
		str += L"\n\nGPU / timing";
		if (m_VendorId == PCIV_NVIDIA) {
			if (!m_gpuLoadSampler) m_gpuLoadSampler = std::make_unique<edition::GpuLoadSampler>();
			const auto gpu = m_gpuLoadSampler->Read();
			const auto value = [](int v) -> std::wstring {
				return v >= 0 ? std::format(L"{}%", v) : L"n/a";
			};
			str += std::format(L"\nGPU activity  {} | Memory bus {}", value(gpu.gpu), value(gpu.memory));
			str += std::format(L"\nEngines       Decode {} | Encode {} | OFA {}",
				value(gpu.decoder), value(gpu.encoder), value(gpu.opticalFlow));
			// This is one NVML signal, not two independent measures.
			str += std::format(L"\nCore idle     {} | 6-sample idle low {}",
				value(gpu.gpu >= 0 ? 100 - gpu.gpu : -1), value(gpu.idleLow));
		} else {
			str += L"\nGPU activity  n/a (NVML: NVIDIA only)";
		}
		// NVML GPU idle is NOT a full pipeline GPU frame deadline measurement.
		// DLSS SR, ZipDepth and DX12 MFG can overlap on independent timelines.
		// Never turn (100 - GPU load) into an invented per-frame headroom.
		if (running && m_dRefreshPeriodMs > 0.0) {
			str += std::format(L"\nFrame budget  {:.2f} ms | GPU frame headroom n/a", m_dRefreshPeriodMs);
		} else {
			str += L"\nGPU frame headroom n/a";
		}
		if (srActive && sr.ngxGpuTimeValid) {
			str += std::format(L"\nDLSS SR GPU   {:.2f} ms (NGX only)", sr.ngxGpuMs);
		}
		if (zipDepthEnabled && depth.gpuTimings.available) {
			str += std::format(L"\nZipDepth GPU  {:.2f} / {:.2f} / {:.2f} ms (pack/model/post)",
				depth.gpuTimings.packMs, depth.gpuTimings.modelMs, depth.gpuTimings.postMs);
		}

		const bool mfgActive = !mfgReason && m_mfgMultiplier && m_mfg.InputSize().cx;
		if (mfgActive || m_completedPresentWaitable) {
			str += L"\nPresent       ";
			str += displayFps ? std::format(L"DXGI {} fps", edition::CompactNumber(*displayFps)) : std::format(L"DXGI n/a ({})", dxgiStatus);
			if (skippedPerSec >= 0.1)
				str += std::format(L" | {} skipped/s", edition::CompactNumber(skippedPerSec));
		}
		if (hasVideoBuffer) {
			str.append(m_completedBufferStats);
		}
		if (m_completedResizeEarlyDrops)
			str += std::format(L"\nMFG resize    {} early drops", m_completedResizeEarlyDrops);
		const auto lostOriginals = m_mfgLostReal.load(std::memory_order_relaxed);
		if (lostOriginals > m_editionOriginalDropsBase)
			str += std::format(L"\nOriginal drops {}", lostOriginals - m_editionOriginalDropsBase);
		if (mfgReason) str += std::format(L"\nMFG           {}", mfgReason);
		if (srSelected && !srActive) {
			str += std::format(L"\nDLSS SR       {}", m_dlssSR.Preparing() ? L"Preparing" :
				m_dlssSRInputStatus ? m_dlssSRInputStatus : L"Inactive");
		}
		if (zipDepthEnabled) {
			const auto cache = m_zipDepth.GetCacheStats(playbackPts);
			str += cache.capacity ?
				std::format(L"\nZipDepth      {}/{} cached | {} late maps", cache.cached, cache.capacity, depth.lateMaps) :
				std::format(L"\nZipDepth      {}", m_zipDepth.Status());
		}
        if (PerceptualEnabled()) {
            str += std::format(L"\nPreferred/Skin {} / {} | Depth {} used / {} fallback",
                m_pFilter->m_videoEnhancement.load() ? L"On" : L"Off",
                m_pFilter->m_videoEnhancement.load() ? L"On" : L"Off",
                m_perceptual.depthUsed, m_perceptual.depthFallback);
            if (m_perceptual.failedStandalone || m_perceptual.failedB)
                str += L"\nPerceptual    shader compile failed (fallback)";
        }
		if (svdRaw) {
			const auto& b = m_svdLutRawTest;
			str += std::format(L"\nVE temporal   {}/{} analyzed | {} graded | {} ungraded",
				b.completed, b.requests, b.presentedGraded, b.presentedUngraded);
            if (svdTemporal && b.grayProbe.valid) {
                const auto& probe = b.grayProbe;
                // Synthetic gray (not sampled from the output), prior to
                // B2.1/Preferred/Skin/Depth/SR/MFG and display conversion.
                const wchar_t* origin = probe.source == 1 ? L"temporal" :
                    probe.source == 2 ? L"held" : L"raw";
                str += std::format(
                    L"\nB1 gray 236   LUT {:.0f}/{:.0f}/{:.0f} | B mix {:.0f}/{:.0f}/{:.0f} ({})",
                    probe.model[0] * 255.0f, probe.model[1] * 255.0f,
                    probe.model[2] * 255.0f,
                    probe.strengthMixed[0] * 255.0f, probe.strengthMixed[1] * 255.0f,
                    probe.strengthMixed[2] * 255.0f, origin);
            } else if (svdTemporal) {
                str += L"\nB1 gray 236   n/a (selected LUT data unavailable)";
            }
            if (svdTemporal && b.b2Analyses) {
                str += std::format(L"\nVE luma         {:.1f}% luma | {:.2f} EV candidate shift",
                    b.b2LastTone[0] * 100.0f, b.b2LastSourceShiftEv);
            }
			if (b.b2MemoryShaderFailed)
				str += L"\nVE chroma     shader compile failed";
			if (b.b2ShaderFailed)
				str += L"\nVE luma       shader compile failed";
			if (b.model.GetState() == svdlut::ModelWorker::State::Failed || b.failed)
				str += std::format(L"\nVE model error {}", SvdLutStatus());
		}
	}

	return separator;
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
		if (UsesZipDepthMfg()) {
			diagnostics += std::format(L"\nMFG depth      : {}", m_mfg.UsesZipDepth() ? L"ZipDepth active" :
				!MfgDepthSourceEligible() ? L"flat (unsupported source)" : m_zipDepth.Status());
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
			str += std::format(L"\nDLSS SR        : {}", m_dlssSR.Status(UsesZipDepthSr() ? &m_zipDepth : nullptr));
			if (m_dlssSR.Active()) {
				str += m_dlssSRAfterMfg ? L" (after MFG)" : L" (source frames)";
				if (m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) str += L" (deinterlaced)";
				const auto stats = m_dlssSR.GetStats();
				if (stats.ngxGpuTimeValid) str += std::format(L"\nDLSS SR NGX GPU: {:.2f} ms", stats.ngxGpuMs);
			}
		}
	}
	if (UsesZipDepthSr() || UsesZipDepthMfg()) {
		const auto& depth = m_zipDepth.GetCounters();
		const auto latest = m_zipDepth.LatestPreview();
		str += std::format(L"\nZipDepth       : {} captures, {} DirectML jobs, {} maps ({} late)"
			L"\nZipDepth usage : {} SR evaluations, {} MFG submissions",
			depth.captures, depth.jobs, depth.maps, depth.lateMaps, depth.srEvaluations, depth.mfgSubmissions);
		if (const auto* precision = m_zipDepth.ModelPrecision(); *precision)
			str += std::format(L"\nZipDepth model : {}", precision);
		if (depth.maps)
			str += std::format(L"\nZipDepth timing: GPU job {:.1f} ms, ready at {:.0f} ms age",
				depth.inferenceMs, double(depth.resultAge) / 10000.0);
		if (depth.gpuTimings.available)
			str += std::format(L"\nZipDepth GPU   : pack {:.2f} ms, model interval {:.2f} ms, post {:.2f} ms",
				depth.gpuTimings.packMs, depth.gpuTimings.modelMs, depth.gpuTimings.postMs);
		if (latest.view)
			str += std::format(L"\nZipDepth latest: {:.0f} ms PTS", double(latest.pts) / 10000.0);
		else
			str += std::format(L"\nZipDepth latest: {}", m_zipDepth.Status());
	}
}
