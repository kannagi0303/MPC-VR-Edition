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
#include "VideoRenderer.h"
#include "Depth/DecodedFrameLookahead.h"
#include "Edition/WindowMessages.h"

// These hooks execute synchronously at the original lifecycle call sites.

// Called from the input pin BEFORE its Receive lock, or from Stop before
// RendererLock. Cancellation is independent of the current source state.
void CMpcVideoRenderer::CancelEditionDelivery() noexcept
{
    m_playbackCancel.Cancel();
    if (m_depthLookahead) m_depthLookahead->Cancel();
    if (m_ThreadSignal) SetEvent(m_ThreadSignal);
}

// EndFlush and a fresh Run/Pause establish a new source timeline. Tickets
// captured by the previous video remain permanently cancelled.
void CMpcVideoRenderer::ResumeEditionDelivery(bool beginPlayback)
{
    // EndFlush can race with Stop: do not re-enable an already stopped source.
    // A fresh Run/Pause from Stopped explicitly starts a new generation.
    if (beginPlayback || m_filterState != State_Stopped)
        m_playbackCancel.Restart();
}

void CMpcVideoRenderer::BeginEditionSegment(double rate)
{
	if (m_depthLookahead) m_depthLookahead->Cancel();
	CAutoLock lock(&m_RendererLock);
	m_completedSegmentRate = rate;
	++m_completedEpoch;
	ResetSourceTime();
	m_completedEndOfStream = false;
	if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) {
		dx11->m_mfgDisplayRate.BreakContinuity();
		dx11->m_editionOriginalDropsBase = dx11->m_mfgLostReal.load(std::memory_order_relaxed);
		dx11->ClearCompletedFrames(false);
	}
}

void CMpcVideoRenderer::BeginEditionFlush()
{
	if (m_depthLookahead) m_depthLookahead->Cancel();
	++m_completedEpoch;
	ResetSourceTime();
	m_completedEndOfStream = false;
	if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) dx11->ClearCompletedFrames(false);
}

void CMpcVideoRenderer::BeginEditionRun()
{
	const bool starting = m_filterState == State_Stopped;
	if (starting) ResumeEditionDelivery(true);
	m_featureStatusPlaybackRun.fetch_add(1, std::memory_order_relaxed);
	m_filterState = State_Running;
	if (starting) {
		CAutoLock cRendererLock(&m_RendererLock);
		BeginStartupOverlay();
	}
	m_bClockStalled = false;
	NoteWhoKeepsTime();
}

void CMpcVideoRenderer::CompleteEditionRun(HRESULT hr)
{
	if (SUCCEEDED(hr)) {
		CAutoLock lock(&m_RendererLock);
		if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) dx11->m_completedPresenter.Wake();
	}
}

FILTER_STATE CMpcVideoRenderer::BeginEditionPause()
{
	const auto previous = m_filterState;
	if (previous == State_Stopped) ResumeEditionDelivery(true);
	m_filterState = State_Paused;
	if (previous == State_Stopped) {
		CAutoLock cRendererLock(&m_RendererLock);
		BeginStartupOverlay();
	}

	return previous;
}

void CMpcVideoRenderer::CompleteEditionPause(HRESULT hr, FILTER_STATE previous)
{
	if (SUCCEEDED(hr) && previous == State_Running) {
		CAutoLock cRendererLock(&m_RendererLock);
		if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get()))
			dx11->InvalidateCompletedBackBuffer();
		if (m_filterState == State_Paused && m_bValidBuffer &&
			m_VideoProcessor->Type() == VP_DX11 && m_Sets.bShowFeatureBadges) {
			Redraw();
		}
	}
}

void CMpcVideoRenderer::BeginEditionStop()
{
	m_filterState = State_Stopped;
	CancelEditionDelivery();
	// Wake a streaming/lookahead thread that may still be waiting for its
	// presentation deadline. Cancel before taking RendererLock so Stop does
	// not spend another scheduling interval waiting for that thread.
	if (m_ThreadSignal) SetEvent(m_ThreadSignal);
	m_bValidBuffer = false;
	m_bStartupOverlayPending.store(false, std::memory_order_release);
	// Stopped is not initialization; don't keep (or resurrect) the label.
	if (m_hWndWindow) PostMessageW(m_hWndWindow, WM_VR_STARTUP_OVERLAY, FALSE, 0);

	{
		CAutoLock cRendererLock(&m_RendererLock);
		++m_completedEpoch;
		ResetSourceTime();
		m_completedEndOfStream = false;
		m_VideoProcessor->Flush();
	}
}

// Called only after a real video frame has successfully reached the swapchain,
// either via the ordinary DX11 renderer or the asynchronous MFG presenter.
// Do not take renderer or UI locks here: both paths can call this on GPU threads.
void CMpcVideoRenderer::OnVideoPresented()
{
	if (m_filterState == State_Stopped) return;
	if (m_bStartupOverlayPending.exchange(false, std::memory_order_acq_rel)) {
		const HWND hwnd = m_hWndWindow;
		if (hwnd) PostMessageW(hwnd, WM_VR_STARTUP_OVERLAY, FALSE, 0);
	}
}

const wchar_t* CMpcVideoRenderer::FeatureStatusText(bool dlssSR, bool vsr, bool mfg)
{
	if (dlssSR) return mfg ? L"DLSS SR | MFG" : L"DLSS SR";
	if (vsr) return mfg ? L"VSR | MFG" : L"VSR";
	return mfg ? L"MFG" : L"";
}

void CMpcVideoRenderer::BeginStartupOverlay()
{
	if (!m_Sets.bWaitForExtendedStartup || !m_Sets.bShowFeatureBadges || m_Sets.bShowStats ||
		m_VideoProcessor->Type() != VP_DX11) return;
	const bool dlssSR = m_Sets.bDlssSR;
	const bool vsr = m_Sets.iVPSuperRes > SUPERRES_Disable &&
		m_Sets.iVPSuperRes != SUPERRES_DLSS_SR;
	const auto* dx11 = static_cast<const CDX11VideoProcessor*>(m_VideoProcessor.get());
	const bool mfg = m_Sets.iMfgMultiplier >= 2 && !dx11->MfgSourceMeetsDisplayRate();
	const auto* text = FeatureStatusText(dlssSR, vsr, mfg);
	if (!*text) return;
	m_startupFeatureText.store(text, std::memory_order_relaxed);
	m_bStartupOverlayPending.store(true, std::memory_order_release);
	if (m_hWndWindow) PostMessageW(m_hWndWindow, WM_VR_STARTUP_OVERLAY, TRUE, 0);
}

void CMpcVideoRenderer::ResizeStartupOverlay(HWND hwnd)
{
	if (!m_hStartupOverlay || GetParent(m_hStartupOverlay) != hwnd) return;
	RECT client{};
	if (!GetClientRect(hwnd, &client)) return;
	// Compatible with older Windows builds; no GetDpiForWindow import needed.
	HDC dc = GetDC(hwnd);
	const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
	if (dc) ReleaseDC(hwnd, dc);
	const int margin = MulDiv(14, dpi, 96);
	const int available = std::max(0, int(client.right - client.left) - 2 * margin);
	const int width = std::min(MulDiv(145, dpi, 96), available);
	const int height = MulDiv(28, dpi, 96);
	SetWindowPos(m_hStartupOverlay, HWND_TOP, margin, margin, width, height,
	             SWP_NOACTIVATE);
}


void CMpcVideoRenderer::ShutdownEditionSource()
{
	CancelEditionDelivery();
	if (m_depthLookahead) {
		m_depthLookahead->Cancel();
		SetEvent(m_ThreadSignal);
		m_depthLookahead->Shutdown();
	}
}

void CMpcVideoRenderer::ReleaseEditionUiState()
{
	RemoveToggleKeyHook();
	if (m_hNoticeTimer) {
		// Waits for a callback that is running: it must not outlive what it draws with.
		DeleteTimerQueueTimer(nullptr, m_hNoticeTimer, INVALID_HANDLE_VALUE);
		m_hNoticeTimer = nullptr;
	}
}
