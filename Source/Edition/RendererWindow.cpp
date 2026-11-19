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
#include "Edition/WindowMessages.h"

// bDisabled: the window being replaced was disabled, and so is this one from the
// start. MPC-HC disables whatever the renderer puts in its video area once the owner
// is set, so that the mouse falls through to its own window, which is the one that
// hides the pointer after two seconds. A replacement made enabled took the mouse for
// itself, and the pointer stayed over the film until it was closed -- upstream's fault
// too, with any setting that has the window made again, the HDR output list first among them.
HWND CMpcVideoRenderer::CreateVideoWindow(const bool bDisabled)
{
	return CreateWindowExW(
		0,
		RendererWindowClassName,
		nullptr,
		WS_VISIBLE | WS_CHILDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | (bDisabled ? WS_DISABLED : 0),
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		m_hWndParent,
		nullptr,
		g_hInst,
		this
	);
}

void CMpcVideoRenderer::ApplyPendingGeometry()
{
	const HWND window = m_hWndWindow;
	if (window && !m_RendererLock.TryLock()) {
		// DXGI may be waiting for this window to process a synchronous message.
		// Coalesce geometry changes and give the message pump back to the player.
		if (!m_geometryPosted.exchange(true) && !PostMessageW(window, WM_VR_GEOMETRY, 0, 0)) {
			m_geometryPosted = false;
		}
		return;
	}
	if (!window) m_RendererLock.Lock();
	struct Unlock { CCritSec& lock; ~Unlock() { lock.Unlock(); } } render{m_RendererLock};
	std::optional<CRect> windowRect, videoRect;
	{
		std::lock_guard lock(m_pendingGeometryLock);
		windowRect.swap(m_pendingWindowRect);
		videoRect.swap(m_pendingVideoRect);
	}
	if (windowRect) ApplyWindowPosition(*windowRect);
	if (videoRect && *videoRect != m_videoRect) {
		m_videoRect = *videoRect;
		m_VideoProcessor->SetVideoRect(*videoRect);
	}
	auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
	const bool presenting = dx11 && m_filterState == State_Running &&
		(dx11->CompletedFramesPending() || dx11->m_completedLast);
	if ((windowRect || videoRect) && m_bForceRedrawing && !presenting) Redraw();
}

std::optional<LRESULT> CMpcVideoRenderer::HandleEditionWindowMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_VR_GEOMETRY) {
		if (!SetTimer(hwnd, VR_GEOMETRY_TIMER, 15, nullptr)) m_geometryPosted = false;
		return 0;
	}
	if (uMsg == WM_TIMER && wParam == VR_GEOMETRY_TIMER) {
		KillTimer(hwnd, VR_GEOMETRY_TIMER);
		m_geometryPosted = false;
		ApplyPendingGeometry();
		return 0;
	}
	if (uMsg == WM_CREATE) {
		// The label is a native child window, not a video/MFG OSD. It can
		// paint on the black area before the first GPU frame exists.
		// A solid child surface (not WS_EX_TRANSPARENT) is needed above the
		// swapchain's black frames. The stock font avoids a separate GDI object
		// and teardown work each time MPC-BE opens/closes a video window.
		m_hStartupOverlay = CreateWindowExW(0, L"STATIC", L"",
			WS_CHILD | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, 0, 0, 0, 0,
			hwnd, nullptr, g_hInst, nullptr);
		if (m_hStartupOverlay) {
			ResizeStartupOverlay(hwnd);
			if (m_bStartupOverlayPending.load(std::memory_order_acquire)) {
				SetWindowTextW(m_hStartupOverlay, m_startupFeatureText.load(std::memory_order_relaxed));
				ShowWindow(m_hStartupOverlay, SW_SHOWNOACTIVATE);
			}
		}
		return 0;
	}
	if (uMsg == WM_SIZE && m_bStartupOverlayPending.load(std::memory_order_acquire)) {
		ResizeStartupOverlay(hwnd);
	}
	if (uMsg == WM_VR_STARTUP_OVERLAY) {
		if (m_hStartupOverlay && GetParent(m_hStartupOverlay) == hwnd) {
			// Use the current state, not a potentially stale queued command.
			const bool show = m_bStartupOverlayPending.load(std::memory_order_acquire);
			if (show) {
				ResizeStartupOverlay(hwnd);
				SetWindowTextW(m_hStartupOverlay, m_startupFeatureText.load(std::memory_order_relaxed));
			}
			ShowWindow(m_hStartupOverlay, show ? SW_SHOWNOACTIVATE : SW_HIDE);
			if (show) SetWindowPos(m_hStartupOverlay, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		return 0;
	}
	if (uMsg == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lParam) == m_hStartupOverlay) {
		HDC dc = reinterpret_cast<HDC>(wParam);
		SetTextColor(dc, RGB(235, 238, 242));
		SetBkColor(dc, RGB(0, 0, 0));
		return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
	}
	if (uMsg == WM_DESTROY && hwnd == m_hWndWindow) {
		m_geometryPosted = false;
		// Windows destroys children automatically. No cross-thread wait or
		// extra DestroyWindow call is needed during player shutdown.
		m_hStartupOverlay = nullptr;
	}
	if (uMsg == WM_VR_REMAKE) {
		return reinterpret_cast<LRESULT>(CreateVideoWindow(wParam != 0));
	}

	return {};
}
