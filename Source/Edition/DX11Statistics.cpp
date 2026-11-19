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

std::optional<size_t> CDX11VideoProcessor::AppendEditionStatistics(std::wstring& str, double sourceFps)
{
	std::wstring mfgStats;
	if (m_mfgMultiplier) {
		if (MfgSourceMeetsDisplayRate()) {
			mfgStats = L"\nMFG      : Bypassed (source meets display refresh)";
		} else if (m_mfgPresentFailed) {
			mfgStats = L"\nMFG      : Presenter failed";
		} else if (m_mfg.Failed()) {
			mfgStats = L"\nMFG      : Unavailable";
		} else if (m_pFilter->m_filterState == State_Paused) {
			mfgStats = L"\nMFG      : Paused";
		} else {
			const auto now=int64_t(GetPreciseTick());
			const auto real=m_mfgRealPresents.load(std::memory_order_relaxed);
			const auto generated=m_mfgGeneratedPresents.load(std::memory_order_relaxed);
			const auto fallback=m_mfgFallbackPresents.load(std::memory_order_relaxed);
			const auto skipped=m_mfgSkipped.load(std::memory_order_relaxed);
			double outputFps = 0.0, skippedPerSec = 0.0;
			{
				std::lock_guard guard(m_mfgRateLock);
				const double frequency=double(GetPreciseTicksPerSecond());
				if (!m_mfgRateTick || now<m_mfgRateTick) {
					m_mfgRateTick=now;
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
				else if (frequency>0 && double(now-m_mfgRateTick)>=frequency) {
					const double seconds=double(now-m_mfgRateTick)/frequency;
					const auto realDelta=real>=m_mfgRateReal?real-m_mfgRateReal:0;
					const auto genDelta=generated>=m_mfgRateGenerated?generated-m_mfgRateGenerated:0;
					const auto fallbackDelta=fallback>=m_mfgRateFallback?fallback-m_mfgRateFallback:0;
					const auto skipDelta=skipped>=m_mfgRateSkipped?skipped-m_mfgRateSkipped:0;
					m_mfgActualFps=double(realDelta+genDelta+fallbackDelta)/seconds;
					if (mfg::SourceCadenceTrace().Enabled()) {
						// Window counts are stored in serial (source), phase (generated),
						// and effective_multiplier (fallback). The unit of duration is QPC.
						mfg::SourceCadenceTrace().Record("mfg_stats_window", now, -1, -1,
							realDelta, unsigned(genDelta), unsigned(fallbackDelta), 0, 0, false,
							m_mfgLostGenerated, S_OK, now - m_mfgRateTick);
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
					if(SUCCEEDED(statsHr)) {
						if(m_mfgDxgiHasBaseline && dxgi.PresentCount>=m_mfgDxgiLastPresentCount) {
							const double rate=double(dxgi.PresentCount-m_mfgDxgiLastPresentCount)/seconds;
							const double displayHz=m_dRefreshPeriodMs>0.0?1000.0/m_dRefreshPeriodMs:0.0;
							// DXGI stats can be discontinuous on mode changes / some display paths.
							// Never report an impossible scan-out rate as a measurement.
							m_mfgDxgiAvailable=displayHz>0.0 && rate<=displayHz*1.2+1.0;
							if(m_mfgDxgiAvailable) m_mfgDxgiDisplayFps=rate;
						} else m_mfgDxgiAvailable=false;
						m_mfgDxgiLastPresentCount=dxgi.PresentCount;
						m_mfgDxgiHasBaseline=true;
					} else {m_mfgDxgiHasBaseline=false;m_mfgDxgiAvailable=false;}
					m_mfgRateTick=now;
					m_mfgRateReal=real;
					m_mfgRateGenerated=generated;
					m_mfgRateFallback=fallback;
					m_mfgRateSkipped=skipped;
				}
				outputFps=m_mfgActualFps;
				skippedPerSec=m_mfgSkippedPerSec;
			}
			const auto inputSize = m_mfg.InputSize();
			const auto outputSize = m_mfg.GeneratedSize();
			if (inputSize.cx && sourceFps > 0.0 && outputFps > 0.0) {
				// This rate counts successful Presents and may differ from scan-out.
				const auto fps = [](double value) {
					auto text = std::format(L"{:.2f}", value);
					while (text.back() == L'0') text.pop_back();
					if (text.back() == L'.') text.pop_back();
					return text;
				};
				mfgStats = std::format(L"\nMFG      : {}x{} {}fps -> {}x{} {}fps",
					inputSize.cx, inputSize.cy, fps(sourceFps), outputSize.cx, outputSize.cy, fps(outputFps));
				if (skippedPerSec >= 0.1) mfgStats += std::format(L" | {:.1f} skipped/s", skippedPerSec);
			} else {
				mfgStats = L"\nMFG      : Waiting for frames";
			}
		}
	}

	std::wstring srStats;
	if (m_iVPSuperRes == SUPERRES_DLSS_SR) {
		const auto stats = m_dlssSR.GetStats();
		if (m_dlssSR.Preparing()) {
			srStats = L"\nDLSS SR  : Preparing";
		} else if (!stats.inputWidth) {
			srStats = L"\nDLSS SR  : Inactive";
			if (m_dlssSRInputStatus) srStats += std::format(L" ({})", m_dlssSRInputStatus);
		} else if (stats.inputWidth == stats.outputWidth && stats.inputHeight == stats.outputHeight) {
			srStats = std::format(L"\nDLAA     : {}x{}", stats.inputWidth, stats.inputHeight);
		} else {
			srStats = std::format(L"\nDLSS SR  : {}x{} -> {}x{}",
				stats.inputWidth, stats.inputHeight, stats.outputWidth, stats.outputHeight);
		}
	}

	std::wstring depthStats;
	if (UsesZipDepthSr() || UsesZipDepthMfg()) {
		const auto& depth = m_zipDepth.GetCounters();
		const auto* precision = m_zipDepth.ModelPrecision();
		if (depth.maps && *precision) {
			depthStats = std::format(L"\nZipDepth : {}, {:.1f} maps/s", precision, depth.mapsPerSecond);
			if (depth.lateMaps) depthStats += std::format(L", {} late", depth.lateMaps);
			const auto now = GetTickCount64();
			const bool srUsed = UsesZipDepthSr() &&
				depth.srEvaluations && now - depth.lastSrUseTick <= 1000;
			const bool mfgUsed = UsesZipDepthMfg() && depth.mfgSubmissions && now - depth.lastMfgUseTick <= 1000;
			if (srUsed || mfgUsed) {
				depthStats += srUsed && mfgUsed ? L" | SR + MFG" : srUsed ? L" | SR" : L" | MFG";
			}
			if (m_zipDepth.CoolingDown(now)) depthStats += L" | paused (late)";
		} else {
			depthStats = std::format(L"\nZipDepth : {}", m_zipDepth.Status());
		}
	}

	std::wstring bufferStats;
	CRefTime bufferStream;
	if ((m_completedLast || CompletedFramesPending()) && SUCCEEDED(m_pFilter->StreamTime(bufferStream))) {
		const auto time = GetCompletedBufferTime(bufferStream);
		bufferStats = std::format(L"\nVideo buffer : {} ms ready / {} ms queued", time.ready / 10000, time.queued / 10000);
		if (time.nonIncreasing) bufferStats += std::format(L" | {} due out of order", time.nonIncreasing);
	}
	const bool hasEditionStats = m_svdLut.strength || !srStats.empty() || !mfgStats.empty() ||
		!depthStats.empty() || !bufferStats.empty();
	const auto separator = str.size() + 1;
	if (hasEditionStats) {
		str += L'\n'; // Reserve a row before the layout computes the text width.
		if (!srStats.empty() && m_dlssSR.Active() && m_dlssSRAfterMfg) {
			str.append(mfgStats).append(srStats);
		} else {
			str.append(srStats).append(mfgStats);
		}
		str.append(depthStats);
		if (m_svdLut.strength) str += std::format(L"\nSVDLUT   : {} ({}%)", SvdLutStatus(), m_svdLut.strength);
		str.append(bufferStats);
	}
	if (hasEditionStats) return separator;
	return {};
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
