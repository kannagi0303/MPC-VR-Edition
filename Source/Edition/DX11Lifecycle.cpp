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
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"
#include "Mfg/SettingsScope.h"

void CDX11VideoProcessor::ApplyJvrPlaybackMode(Settings_t& config, bool projectJvr)
{
	if (!projectJvr) return;

	// The decoder VP must deliver source colour for projection. Its RTX
	// enhancements would operate on both eyes before the RGB source exists.
	config.iVPSuperRes = SUPERRES_Disable;
	config.bVPRTXVideoHDR = false;
}

CDX11VideoProcessor::EditionConfiguration CDX11VideoProcessor::ConfigureEdition(Settings_t& config)
{
	InvalidateCompletedBackBuffer();
	const bool informationChanged = m_showFeatureStatus.load(std::memory_order_relaxed) != config.bShowFeatureBadges;
	if (informationChanged || m_showEditionDetails != config.bShowEditionDetails) {
		m_cachedStatisticsTick = 0;
		m_completedBufferStatsTick = 0;
		m_completedBufferStats.clear();
		m_StatsLines = 19;
		m_StatsColumns = kStatsMinColumns;
		m_StatsShrinkCount = 0;
	}
	m_showEditionDetails = config.bShowEditionDetails;
	mfg::EnforceScope(config);
	const bool jvrFlatView = m_pFilter->JvrProjectionRequested(config);
	ApplyJvrPlaybackMode(config, jvrFlatView);
	const int superRes = config.bDlssSR ? SUPERRES_DLSS_SR : config.iVPSuperRes;
	const unsigned mfgMultiplier = config.bUseD3D11 ? config.iMfgMultiplier : 0;
	const bool srWasEnabled = m_iVPSuperRes == SUPERRES_DLSS_SR;
	const bool srChanged = srWasEnabled != config.bDlssSR ||
		(config.bDlssSR && m_dlssSRSourceLimit != config.iDlssSRSourceLimit);
	if (m_iVPSuperRes != superRes) m_editionVsr = {};
	const bool wasUsingDepth = UsesDepthSr() || UsesDepthMfg();
	const bool useDepth = (config.bUseZipDepthSr && config.bDlssSR) ||
		(config.bUseZipDepthMfg && mfgMultiplier);
	const bool overlayChanged = (wasUsingDepth && m_showDepthOverlay) !=
		(useDepth && config.bShowDepthOverlay);
	const unsigned processingMode = config.iMfgProcessingMode <= 1 ? config.iMfgProcessingMode : 0;
    if ((srChanged || m_dlssSRDlaa != config.bDlssSRDlaa ||
        m_mfgProcessingMode != processingMode) &&
        (m_earlyOriginal.srSource || m_futureOriginal.srSource)) {
        DiscardEarlyOriginal(m_futureOriginal);
        m_futureOriginal = {};
        FinishEarlyOriginal();
    }
	const bool jvrModeChanged = jvrFlatView != m_jvrFlatView;
	if (processingMode != m_mfgProcessingMode ||
		(m_mfgMultiplier && (srChanged || overlayChanged || jvrModeChanged) && m_pFilter->GetActive())) {
		SwitchMfgUpscaleSlot(processingMode);
	} else if (overlayChanged) {
		m_dlssSR.ResetHistory();
	}
	// Only a change of the independent OSD setting restarts its session.
	// Result changes are observed by DrawFeatureStatus; geometry/history changes do not rearm it.
	if (m_showEditionOsd.load(std::memory_order_relaxed) != !config.bHideEditionOsd) ResetFeatureStatus();
	m_jvrFlatView = jvrFlatView;
	ConfigureJvrProjection(config);
	if (jvrModeChanged) {
		m_featureProjectionReady = false;
		m_depth.StopLookahead();
		m_depth.ResetHistory();
		if (!jvrFlatView) {
			m_jvrSource.Release();
			m_jvrFrame.Release();
		}
	}
	m_rtVideoDelay=config.iVideoDelay*10000LL;
	// MFG cannot remain active on an existing D3D11 instance after the
	// user disables D3D11. Preserve the saved selection in Settings_t.
	if (m_mfgLowMotionEnabled != config.bMfgLowMotion) {
		if (m_mfgPendingRepeats.load(std::memory_order_relaxed) || m_mfgRepeatHold) m_mfg.ResetHistory();
		ResetMfgFrameAnalysis();
		m_mfgRepeatTiming.ClearEvidence();
		m_mfgDecodedDifference = {};
		m_mfgRepeatsBypassed = 0;
		m_mfgLowMotionSaved = 0;
		m_mfgLowMotionVetoes = 0;
		m_mfgLowMotionEnabled = config.bMfgLowMotion;
	}
	if (m_mfgMultiplier != mfgMultiplier) {
		// There is no MFG session to stop when enabling for the first time.
		// NGX initialization stays lazy: QueueMfg() is entered only after a
		// valid video frame arrives on the active DirectShow render path.
		if (m_mfgMultiplier) {
			if (mfgMultiplier && !m_mfg.Failed()) {
				StopMfg(false);
				m_mfg.ResetHistory();
			} else {
				StopMfg();
			}
		}
		m_mfgMultiplier = mfgMultiplier;
		m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
	}
	if (jvrModeChanged) ResetSvdLutHistory();
	if (m_useDepthSr != config.bUseZipDepthSr) m_dlssSR.ResetHistory();
	if (m_useDepthMfg != config.bUseZipDepthMfg)
		m_mfg.ResetDepthHistory();
	m_useDepthSr = config.bUseZipDepthSr;
	m_useDepthMfg = config.bUseZipDepthMfg;
	m_showMfgMotion = config.bShowMfgMotion;
	m_showDepthOverlay = config.bShowDepthOverlay;
	m_depth3DPreview = config.bDepth3DPreview;
	if (!m_showDepthOverlay || !useDepth) m_depthOverlayCapture.Release();
	if (wasUsingDepth && !useDepth) m_depth.Stop();
	return {superRes, srChanged, jvrModeChanged};
}

void CDX11VideoProcessor::ResetEditionVideoProcessor()
{
	m_editionVsr = {};
	m_editionOriginalDropsBase = m_mfgLostReal.load(std::memory_order_relaxed);
	m_depth.ResetHistory();
	m_dlssSRAfterMfg = false;
    if (m_earlyOriginal.srSource || m_futureOriginal.srSource) {
        DiscardEarlyOriginal(m_futureOriginal);
        m_futureOriginal = {};
        FinishEarlyOriginal();
    }
	m_dlssSR.Reset();
	m_dlssSRSource.Release();
	m_depthOverlayCapture.Release();
    // These independent VPs were configured against the previous media/device
    // domain, even if the new source has the same format and dimensions.
    m_adaptiveTone.sourceCscVP.ReleaseVideoDevice();
    m_adaptiveTone.sourceVsrVP.ReleaseVideoDevice();
    m_adaptiveTone.sourceVsrKey = {};
    m_adaptiveTone.sourceVsrReady = false;
    // A media-format change also invalidates the dedicated post-MFG VP.
    if(m_mfgPostVsrReady || m_mfgPostVsrBroken) StopMfg();
}

void CDX11VideoProcessor::StopEditionDevice()
{
	ClearCompletedFrames(true);
	StopMfg(true, m_fastShutdown);
	m_depth.Stop(m_fastShutdown);
	ReleaseSvdLut();
}

void CDX11VideoProcessor::ReleaseEditionProjectionResources()
{
	m_jvrShader.Release();
	m_jvrConstants.Release();
	m_jvrShaderFailed = false;
}

void CDX11VideoProcessor::ReleaseEditionOverlayResources()
{
	m_pPS_DepthOverlay.Release();
	m_pDepthOverlayBlendState.Release();
	m_pDepthOverlayConstants.Release();
	m_pVS_Depth3DPreview.Release();
	m_pPS_Depth3DPreview.Release();
	m_pVDepth3DPreviewInputLayout.Release();
	m_pDepth3DPreviewConstants.Release();
	m_pDepth3DPreviewVertexBuffer.Release();
	m_pDepth3DPreviewIndexBuffer.Release();
	m_pDepth3DPreviewRasterizer.Release();
	m_depth3DPreviewIndexCount = 0;
	m_depth3DPreviewTexture.Release();
	m_pMfgMotionShader.Release();
	m_pMfgMotionBlend.Release();
	m_pMfgMotionConstants.Release();
	m_depthOverlayShaderFailed = false;
	m_depth3DPreviewShaderFailed = false;
}

void CDX11VideoProcessor::FlushEdition()
{
	// A real graph Stop (including switching media) is terminal for this
	// playback session. Unlike Seek/BeginFlush, it must not wait for model
	// warmup or inference workers to finish before the player can continue.
	const bool stopping = m_pFilter->m_filterState == State_Stopped;
	if (stopping) {
		m_startupHasPicture = false;
		m_extendedStartupPending = false;
		m_mfgDisplayRate = {};
	} else {
		m_mfgDisplayRate.BreakContinuity();
	}
	ClearCompletedFrames(false);
	ResetSvdLutHistory();
	m_depth.StopLookahead();
	m_depth.ResetHistory();
	m_dlssSR.ResetHistory();
	if (stopping) {
		// StopMfg's existing fast path retires unfinished NGX GPU work and
		// abandons in-flight warmup without joining its worker. The other
		// model workers already support safe asynchronous retirement, too.
		// Normal Seek keeps the old history-only behavior below.
		StopMfg(true, true);
		m_depth.Stop(true);
		ReleaseSvdLut(true);
	} else {
		StopMfg(false);
		m_mfg.ResetHistory();
	}
	m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
}


void CDX11VideoProcessor::InitializeEdition(Settings_t& config)
{
	mfg::EnforceScope(config);
	m_showEditionDetails = config.bShowEditionDetails;
	m_jvrFlatView = m_pFilter->JvrProjectionRequested(config);
	ApplyJvrPlaybackMode(config, m_jvrFlatView);
	ConfigureJvrProjection(config);
	m_mfgMultiplier=(config.bUseD3D11 ? config.iMfgMultiplier : 0);m_rtVideoDelay=config.iVideoDelay*10000LL;
    m_mfgProcessingMode = config.iMfgProcessingMode <= 1 ? config.iMfgProcessingMode : 0;
	m_useDepthSr = config.bUseZipDepthSr;
	m_useDepthMfg = config.bUseZipDepthMfg;
	m_showMfgMotion = config.bShowMfgMotion;
	m_showDepthOverlay = config.bShowDepthOverlay;
	m_depth3DPreview = config.bDepth3DPreview;
	m_mfgLowMotionEnabled = config.bMfgLowMotion;
}

void CDX11VideoProcessor::BeginEditionShutdown()
{
	m_fastShutdown = true;
	m_completedPresenter.Stop();
}

void CDX11VideoProcessor::BeginEditionSample(IMediaSample* pSample)
{
	// The decoder can explicitly mark a discontinuity even when timestamps
	// remain monotonic (e.g. a stream splice or a new segment). Never allow
	// DLSS SR to reproject history across that boundary. Restart duplicate
	// detection and interpolation history after a splice or new segment.
	if (pSample->IsDiscontinuity() == S_OK) {
		if (m_mfgMultiplier) {
			StopMfg(false);
			m_mfg.ResetHistory();
			m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
		}
		m_dlssSR.ResetHistory();
		m_depth.ResetHistory();
		m_D3D11VP.ResetFrameOrder();
        // An explicit decoder splice is not a normal missing LUT. Reuse the
        // existing invalidation path only if this timeline has source history.
        if (m_svdLutRawTest.sourceTimeline.current >= 0 && m_svdLutRawTest.attempted)
            ResetSvdLutTemporalQueue();
        m_svdLutRawTest.sourceTimeline = {};
        // A decoder-declared splice is authoritative for the shot anchor,
        // even if its RGB thumbnails look similar and PTS stay monotonic.
        ResetAdaptiveTone();
	}
    REFERENCE_TIME pts = INVALID_TIME, end = INVALID_TIME;
    const bool valid = pSample->GetTime(&pts, &end) == S_OK && pts >= 0 && end > pts;
    if (m_earlyOriginal.ready) {
        auto& e = m_earlyOriginal;
        if (valid && e.sample == pSample && e.pts == pts && e.epoch == m_pFilter->m_completedEpoch &&
            m_completingSample && m_completedTarget && m_completedTarget->videoOnly &&
            !m_pFilter->m_playbackCancel.Capture()->Cancelled()) {
            e.approved = true;
        } else FinishEarlyOriginal();
    }
    m_svdLutRawTest.sourceTimeline.Observe(valid ? pts : INVALID_TIME);
}
