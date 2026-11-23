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
#include "Edition/VideoFrame.h"

// Prepare source projection, buffered-frame routing and the target without
// changing renderer lock scopes or the native HDR/overlay/presentation order.
std::optional<HRESULT> CDX11VideoProcessor::PrepareEditionRender(edition::VideoFrame& frame,
	CComPtr<ID3D11Texture2D>& pBackBuffer, uint64_t& tick1, HRESULT& hr)
{
    // Once MPC-BE requests Stop/Flush, do not launch any new DLSS, depth,
    // LUT, or presentation work from the old source timeline.
    if (m_pFilter->m_playbackCancel.Capture()->Cancelled()) return S_OK;
	const int field = frame.field;
	const auto frameStartTime = frame.frameStartTime;
	const bool mfgConfigured = m_mfgMultiplier.load(std::memory_order_relaxed) >= 2;
	const bool mfgBypassed = mfgConfigured && MfgSourceMeetsDisplayRate();
	const bool mfgEnabledForRate = mfgConfigured && !mfgBypassed;
	if (field && mfgBypassed && m_mfgInputModeKnown) {
		// Scheduled input has drained legacy originals. Completed originals
		// already own their surfaces and retain their existing deadlines.
		StopMfg(false);
		if (!m_mfg.Failed()) m_mfg.ResetHistory();
		m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
		m_mfgInputModeKnown = false;
	}

	// Buffered video-only frames apply the window size at presentation.
	if (!m_completingSample) {
		if (field || !m_completedLast) {
			const auto resizeHr = ApplyPendingWindowResize();
			if (FAILED(resizeHr)) return resizeHr;
		}
		UpdateProcessingResolution();
	}
	if (!field && m_completedLast && (!CanProjectJvr() || m_pFilter->m_filterState == State_Running ||
	    m_completedLast->pts != m_rtStart)) {
		return PresentCompletedVideo(*m_completedLast);
	}
	if (field && !m_completingSample && !CompletedFramesPending()) m_completedLast.reset();
	const bool detached = m_processingSize.cx > 0;
	if (!field && detached && mfgEnabledForRate && m_mfgInputModeKnown &&
	    !m_mfg.Failed() && !m_mfgPresentFailed && m_pFilter->m_filterState == State_Running) {
		// The presenter owns the current picture and will draw with the new window geometry.
		return S_OK;
	}
	const bool completedVideoOnly = m_completedTarget && m_completedTarget->videoOnly;
	if (field) {
		m_FieldDrawn = field;
	}
	const bool second = m_FieldDrawn == 2;
	const bool wantsProjection = CanProjectJvr() && !m_windowRect.IsRectEmpty() && !m_videoRect.IsRectEmpty();
	const CRect sourceRect = ProcessingSourceRect(wantsProjection);
	tick1 = GetPreciseTick();
	const auto projectionHr = wantsProjection ? PrepareJvrFrame(sourceRect, second) : S_FALSE;
	const bool projected = wantsProjection && SUCCEEDED(projectionHr);
	const auto projectionTicks = wantsProjection ? GetPreciseTick() - tick1 : 0;
	DLogIf(FAILED(projectionHr), L"JVR source preparation failed ({}); showing decoded source", HR2Str(projectionHr));
	const bool sourceReady = !wantsProjection || projected;
	const CRect frameSource = projected ? sourceRect : m_srcRect;
	const CRect outputPicture = m_completedTarget ? m_completedTarget->picture : PictureForPlayback(projected);
	const CRect processingRect = detached ? ProcessingRect() : outputPicture;
	if (m_completedTarget) m_completedTarget->projected = projected;

	if (m_completedTarget) {
		pBackBuffer = m_completedTarget->image.pTexture;
		m_completedTarget->pts = frameStartTime;
		m_completedTarget->end = frameStartTime + m_sampleDuration;
		m_completedTarget->due = frameStartTime + m_rtVideoDelay;
	} else {
		InvalidateCompletedBackBuffer();
		hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::Render() : GetBuffer() failed with error {}", HR2Str(hr));
			return hr;
		}
	}

	frame = edition::VideoFrame{
		.field = field, .frameStartTime = frameStartTime,
		.mfgEnabledForRate = mfgEnabledForRate, .detached = detached,
		.completedVideoOnly = completedVideoOnly, .second = second,
		.wantsProjection = wantsProjection, .projected = projected,
		.projectionTicks = projectionTicks, .sourceReady = sourceReady,
		.frameSource = frameSource, .outputPicture = outputPicture, .processingRect = processingRect
	};
	return {};
}

// Called under the same renderer lock as Render(). A result means that the
// frame was queued, completed, or failed; no result continues native composition.
std::optional<HRESULT> CDX11VideoProcessor::RenderEditionVideo(CComPtr<ID3D11Texture2D>& pBackBuffer,
	const edition::VideoFrame& frame, HRESULT& hr)
{
    if (m_pFilter->m_playbackCancel.Capture()->Cancelled()) return S_OK;
	const auto& [field, frameStartTime, mfgEnabledForRate, detached, completedVideoOnly,
		second, wantsProjection, projected, projectionTicks, sourceReady,
		frameSource, outputPicture, processingRect] = frame;
	const bool playFirst = !m_pFilter->m_Sets.bWaitForExtendedStartup;
	const bool wantSr = sourceReady && CanDlssSRUpscale(frameSource, processingRect, projected);
	if (projected && field && frameStartTime >= 0) {
		if (MfgFrameAnalysisEligible()) {
			m_mfgDecodedDifference.Submit(m_pDevice, m_pDeviceContext, m_jvrFrame.pTexture, 0, frameSource, frameStartTime);
		}
		if ((UsesZipDepthSr() && wantSr) || UsesZipDepthMfg()) {
			m_zipDepth.Update(m_pDevice, m_pDeviceContext, m_jvrFrame.pShaderResource,
				frameSource, frameSource.Size(), frameStartTime);
		}
	}
	if (m_svdLut.strength && sourceReady && field == 1) {
		CaptureSvdLut(frameStartTime, projected, second);
	}
	const bool warmZipDepth = (UsesZipDepthMfg() && MfgDepthSourceEligible()) ||
		(UsesZipDepthSr() && wantSr);
	if (field && frameStartTime != INVALID_TIME && warmZipDepth && (playFirst || mfgEnabledForRate)) {
		m_zipDepth.Prepare(m_pDevice);
	}
	const bool canEnhance = !playFirst || (m_startupHasPicture && (!warmZipDepth || !m_zipDepth.Loading()));
	const bool pausedMfg = sourceReady && mfgEnabledForRate && field && frameStartTime != INVALID_TIME &&
		m_pFilter->m_filterState == State_Paused && !m_pPSHalfOUtoInterlace &&
		!m_mfg.Failed();
	const bool prerollMfg = pausedMfg && !playFirst;
	const bool canQueueMfg = sourceReady && mfgEnabledForRate && field && frameStartTime != INVALID_TIME &&
		m_pFilter->m_filterState == State_Running && !m_pFilter->m_bClockStalled && !m_pPSHalfOUtoInterlace;
	const bool prepareMfg = canQueueMfg || pausedMfg;
	auto mfgReady = [&](ID3D11Texture2D* input) {
		return !playFirst || m_mfg.PollWarmup(m_pDevice, m_pDeviceContext, input);
	};
	bool useMfg = canQueueMfg && canEnhance;
	bool nativeWarmupSelected = false;
	bool nativeWarmupPending = false;
	m_extendedStartupPending = playFirst && (!canEnhance && (prepareMfg || wantSr));
	// Fast path is deliberately conservative.  D3D11 VP (or the original color
	// conversion shader) reconstructs chroma/CSC at native video dimensions.
	// MFG then interpolates that RGB image, and the regular resizer runs once
	// for *both* real and generated pictures in CompositeMfg().  Never bypass
	// RTX VSR/HDR, user post shaders, dithering, rotation or interlacing.
	// Auto x1 still needs the same source-first path for its original frame.
	// Switching to legacy merely because no interpolation is needed would flush
	// pending real frames and invert the source timeline.
    const bool postUpscaleRequested = prepareMfg && m_iVPSuperRes != SUPERRES_Disable && MfgPostUpscaleRequested();
    const bool postVsrCandidate = postUpscaleRequested && m_bVPUseSuperRes;
    const bool postDlssSRCandidate = postUpscaleRequested && CanDlssSRUpscale(frameSource, processingRect, projected);
    // Native-size DLAA can be evaluated after MFG without an enlargement.
    const bool postNativeDlaa = postDlssSRCandidate &&
        processingRect.Size() == frameSource.Size();
	const bool nativeMfg = prepareMfg && (!m_resizePending || detached) && !m_mfg.Failed() &&
		!m_mfgPresentFailed.load(std::memory_order_relaxed) && (completedVideoOnly || projected || !m_renderRect.IsRectEmpty()) &&
		!frameSource.IsRectEmpty() && (frameSource.Height() <= 1440 || postNativeDlaa) &&
		((processingRect.Width() > frameSource.Width() && processingRect.Height() > frameSource.Height()) || postNativeDlaa) &&
		((!m_bInterlaced && !m_bDoubleFrames && m_SampleFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) || postDlssSRCandidate) &&
		m_iRotation == 0 && !m_bFlip &&
		!SourceIsHDR() && !m_bVPRTXVideoHDR && !m_bVPUseRTXVideoHDR &&
        ((m_iVPSuperRes == SUPERRES_Disable && !m_bVPUseSuperRes) || postVsrCandidate || postDlssSRCandidate) &&
		GetPostScaleSteps() == 0 &&
		m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM &&
		(projected || (m_D3D11VP.IsReady() ? m_D3D11OutputFmt : m_InternalTexFmt) == DXGI_FORMAT_B8G8R8A8_UNORM);
	if (nativeMfg) {
		// No global preference changes: MPC-VR's normal VP/VSR settings remain
		// authoritative.  Set MPCVR_MFG_NATIVE_INPUT=0 to A/B the legacy path.
		static const bool nativeEnabled = [] {
			wchar_t setting[8]{};
			return GetEnvironmentVariableW(L"MPCVR_MFG_NATIVE_INPUT", setting, _countof(setting)) == 0 || setting[0] != L'0';
		}();
        // Post-MFG VSR is opt-in. A failed initialization MUST keep the
        // original VSR-before-MFG path, not silently use Shader scaling.
        auto& nativeInput = projected ? m_jvrFrame : m_mfgNativeInput;
        nativeWarmupSelected = nativeEnabled && (!postVsrCandidate || EnsureMfgPostVsr()) &&
            (projected || SUCCEEDED(nativeInput.CheckCreate(m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM,
                frameSource.Width(), frameSource.Height(), Tex2D_DefaultShaderRTarget)));
        if (nativeWarmupSelected) {
            nativeWarmupPending = !mfgReady(nativeInput.pTexture);
            m_extendedStartupPending |= nativeWarmupPending;
            if (postDlssSRCandidate && prerollMfg) {
                const CRect nativeRect(0, 0, frameSource.Width(), frameSource.Height());
                PrepareDlssSR(nativeRect, processingRect, true);
            }
        }
        if (nativeWarmupSelected &&
            !nativeWarmupPending && (prerollMfg || (useMfg && !m_mfg.Failed()))) {
            const CRect nativeRect(0, 0, frameSource.Width(), frameSource.Height());
            const auto nativeStart=int64_t(GetPreciseTick());
            HRESULT nativeHr=E_FAIL;
            if (projected) {
                nativeHr = S_OK;
            } else if (postVsrCandidate) {
                // Copy/CSC through a separate VP that has no RTX VSR enabled.
                // The regular decoder VP is never reconfigured.
                const auto held=m_D3D11VP.HoldFrame();
                if(held.pTexture) nativeHr=m_mfgPreVsrVP.ProcessExternal(
                    held.pTexture,held.ArraySlice,m_mfgNativeInput.pTexture,m_srcRect,nativeRect);
                if(FAILED(nativeHr)) {
                    m_mfgPostVsrBroken=true;
                    DLog(L"Hybrid MFG pre-VSR source conversion failed: {}; keeping Golden",HR2Str(nativeHr));
                }
            } else {
                nativeHr = m_D3D11VP.IsReady()
                    ? D3D11VPPass(m_mfgNativeInput.pTexture,m_srcRect,nativeRect,m_FieldDrawn==2)
                    : Process(m_mfgNativeInput.pTexture,m_srcRect,nativeRect,m_FieldDrawn==2);
            }
            const auto nativeEnd=int64_t(GetPreciseTick());
            if (nativeEnd > nativeStart)
                m_mfgVideoTicks.fetch_add(uint64_t(nativeEnd-nativeStart) + projectionTicks, std::memory_order_relaxed);
            m_mfgVideoCount.fetch_add(1,std::memory_order_relaxed);
            if (SUCCEEDED(nativeHr)) {
                if (prerollMfg) {
                    // Create the NGX feature/flow/banks while the DirectShow
                    // clock is still stopped. Never enqueue a preroll frame.
                    const auto ticket = m_pFilter->m_playbackCancel.Capture();
                    if (!ticket->Cancelled()) {
                        m_mfg.SetCancellation(ticket);
                        m_mfg.Warmup(m_pDevice, m_pDeviceContext, nativeInput.pTexture, 2);
                    }
                }
                bool nativeOverlayHandled = false;
                if (useMfg && QueueMfg(nativeInput.pTexture, frameStartTime, nativeOverlayHandled, true, postVsrCandidate, false, projected)) {
                    m_bVPScalingUseShaders=m_D3D11VP.IsReady() && !postVsrCandidate;
                    return S_OK;
                }
            } else {
                nativeWarmupSelected = false;
            }
        }
	}

	bool videoOnly = false;
	bool overlayInputHandled = false;
	// Redraws keep the current PTS. The projected preview uses the newly
	// sampled surface directly instead of SR's cached result for that PTS.
	const auto timestamp = frameStartTime == INVALID_TIME
		? m_rtStart + (second ? m_sampleDuration / 2 : 0)
		: frameStartTime;
	auto processPicture = [&](ID3D11Texture2D* target, const CRect& rect, CTex2DRing* postScale) {
		const bool canWarm = !nativeWarmupSelected && prepareMfg && (!m_resizePending || postScale);
		const bool ready = !nativeWarmupPending && (!canWarm || mfgReady(target));
		m_extendedStartupPending |= playFirst && !ready;
		useMfg = !nativeWarmupSelected && canQueueMfg && canEnhance && ready;
		const bool srAfterNativeMfg = nativeWarmupSelected && postDlssSRCandidate &&
			(prerollMfg || (playFirst && !m_mfg.Failed()));
		const bool allowSr = sourceReady && canEnhance && ready && !srAfterNativeMfg && (field || !projected);
		const auto result = Process(target, frameSource, rect, second, allowSr, timestamp, postScale,
			&overlayInputHandled, projected ? &m_jvrFrame : nullptr);
		m_extendedStartupPending |= playFirst && m_dlssSR.Preparing();
		return result;
	};
	if (completedVideoOnly || projected || !m_renderRect.IsRectEmpty()) {
		const auto videoStart=int64_t(GetPreciseTick());
		if (completedVideoOnly) {
			hr = processPicture(pBackBuffer, outputPicture, &m_processingPostScale);
			if (FAILED(hr)) return hr;
		} else if (detached && SUCCEEDED(m_processingTexture.CheckCreate(m_pDevice, m_SwapChainFmt,
			processingRect.Width(), processingRect.Height(), Tex2D_DefaultShaderRTarget))) {
			const auto steps = GetPostScaleSteps();
			if (SUCCEEDED(m_processingPostScale.CheckCreate(m_pDevice, m_InternalTexFmt,
				processingRect.Width(), processingRect.Height(), steps))) {
				hr = processPicture(m_processingTexture.pTexture, processingRect, &m_processingPostScale);
				videoOnly = SUCCEEDED(hr);
			}
		}
		if (!completedVideoOnly && !videoOnly) {
			hr = processPicture(pBackBuffer, outputPicture, nullptr);
			if (FAILED(hr)) return hr;
		}
		if (mfgEnabledForRate) {
			const auto videoEnd=int64_t(GetPreciseTick());
			if (videoEnd > videoStart)
				m_mfgVideoTicks.fetch_add(uint64_t(videoEnd-videoStart) + projectionTicks, std::memory_order_relaxed);
			m_mfgVideoCount.fetch_add(1,std::memory_order_relaxed);
		}
	}

	// Legacy pre-MFG path: preroll the same full/display-sized input that
	// the running producer will use, without starting its presentation thread.
	const bool videoOnlyInput = videoOnly || completedVideoOnly;
	if (prerollMfg && (!m_resizePending || videoOnlyInput) &&
	    !m_mfg.InputSize().cx) {
		auto* mfgInput = videoOnly ? m_processingTexture.pTexture.p : pBackBuffer.p;
        if (mfgInput) {
            const auto ticket = m_pFilter->m_playbackCancel.Capture();
            if (!ticket->Cancelled()) {
                m_mfg.SetCancellation(ticket);
                m_mfg.Warmup(m_pDevice, m_pDeviceContext, mfgInput, 2);
            }
        }
	}
	// Legacy path preserves VSR/HDR and all other native renderer features.
	// A back-buffer-sized input may need to fall back while its resize is pending.
	auto* pictureTexture = videoOnly ? m_processingTexture.pTexture.p : pBackBuffer.p;
	if (useMfg && (!m_resizePending || videoOnlyInput)) {
		if (QueueMfg(pictureTexture, frameStartTime, overlayInputHandled, false, false, videoOnlyInput, projected)) return S_OK;
	} else if (sourceReady && mfgEnabledForRate && !m_resizePending && m_pFilter->m_filterState == State_Running) StopMfg(false);
	if (!m_completedTarget && (videoOnly || projected || !m_renderRect.IsRectEmpty())) {
		DrawSvdLut(pictureTexture, videoOnly ? processingRect : outputPicture, timestamp);
	}
	if (projected && DepthOverlayEnabled() && !overlayInputHandled) {
		DrawDepthOverlay(pictureTexture, videoOnlyInput ? processingRect : outputPicture, timestamp);
	} else if (!wantsProjection && !overlayInputHandled && (videoOnly || m_renderRect == m_videoRect)) {
		BlendSourceDepthOverlay(pictureTexture, videoOnly ? m_processingTexture.pShaderResource.p : nullptr,
			videoOnly ? processingRect : outputPicture, timestamp);
	}
	if (videoOnly) {
		hr = ResizeShaderPass(m_processingTexture, pBackBuffer, processingRect, outputPicture, 0);
	}
	if (m_completingSample && m_completedTarget) {
		if (FAILED(hr)) return hr;
		if (!m_completedBatches.empty()) {
			m_completedBatches.push_back({std::move(*m_completedTarget), {}});
			m_completedPresenter.Wake();
			m_startupHasPicture = true;
			return S_OK;
		}
		if (!QueueCompletedFrame(std::move(*m_completedTarget))) return E_FAIL;
		m_pDeviceContext->Flush();
		m_startupHasPicture = true;
		return S_OK;
	}
	return {};
}
