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

HRESULT CDX11VideoProcessor::CreateEditionWindowedSwapChain(DXGI_SWAP_CHAIN_DESC1& desc)
{
    if (m_mfgMultiplier.load(std::memory_order_relaxed) >= 2 && desc.BufferCount == 2 &&
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
        auto paced = desc;
        paced.BufferCount = 3;
        paced.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        CComPtr<IDXGISwapChain1> chain;
        CComPtr<IDXGISwapChain2> pacing;
        if (SUCCEEDED(m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &paced, nullptr, nullptr, &chain)) &&
            SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&pacing))) && SUCCEEDED(pacing->SetMaximumFrameLatency(1))) {
            m_completedPresentWaitable = pacing->GetFrameLatencyWaitableObject();
            if (m_completedPresentWaitable) {
                m_pDXGISwapChain1 = chain;
                desc = paced;
                return S_OK;
            }
        }
        if (chain) {
            // A flip chain for the same HWND cannot coexist with the fallback.
            pacing.Release();
            chain.Release();
            m_pDeviceContext->Flush();
        }
    }
    return m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc, nullptr, nullptr, &m_pDXGISwapChain1);
}

UINT CDX11VideoProcessor::EditionSwapChainFlags() const
{
    // DXGI requires resizing a waitable chain with the same waitable flag.
    return m_completedPresentWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
}

void CDX11VideoProcessor::InvalidateCompletedBackBuffer(bool externalPresent)
{
    m_completedBackBuffer = {};
    if (externalPresent) m_completedPresentPermit = false;
}

bool CDX11VideoProcessor::TryCompletedPresentPermit()
{
    if (!m_completedPresentWaitable || m_completedPresentPermit) return true;
    m_completedPresentPermit = WaitForSingleObject(m_completedPresentWaitable, 0) == WAIT_OBJECT_0;
    return m_completedPresentPermit;
}

CRect CDX11VideoProcessor::ProcessingRect() const
{
	return m_processingSize.cx > 0 ? CRect(CPoint(0, 0), m_processingSize) : PictureForPlayback(CanProjectJvr());
}

void CDX11VideoProcessor::UpdateProcessingResolution()
{
	const auto previous = m_processingSize;
	if ((m_iVPSuperRes == SUPERRES_Disable && (!m_mfgMultiplier || MfgSourceMeetsDisplayRate())) || m_pPSHalfOUtoInterlace) {
		m_processingSize = CSize(0, 0);
		if (previous.cx > 0) {
			m_processingTexture.Release();
			m_processingPostScale.Release();
			if (m_D3D11VP.IsReady() && m_bVPScaling) UpdateTexures();
		}
		return;
	}
	if (m_videoRect.IsRectEmpty() || m_srcRectWidth <= 0 || m_srcRectHeight <= 0) {
		return;
	}

	const auto& display = m_pFilter->m_DisplayConfig;
	const bool projected = CanProjectJvr();
	const double ratio = projected ? double(jvr::AspectWidth) / jvr::AspectHeight : m_srcAspectRatioX && m_srcAspectRatioY
		? double(m_srcAspectRatioX) / m_srcAspectRatioY : double(m_srcRectWidth) / m_srcRectHeight;
	const bool rotated = m_iRotation % 180 != 0;
	const double aspect = rotated ? 1.0 / ratio : ratio;
	if (display.width && display.height) {
		// Fit the display without adding letterbox bars to the inference input.
		const auto width = std::min<double>(display.width, display.height * aspect);
		m_processingSize = CSize(std::max(1L, LONG(std::lround(width))),
			std::max(1L, LONG(std::lround(width / aspect))));
	} else {
		// Keep a window-independent canvas even without display mode information.
		const LONG height = projected ? ProcessingSourceRect(true).Height() : rotated ? m_srcRectWidth : m_srcRectHeight;
		m_processingSize = CSize(std::max(1L, LONG(std::lround(height * aspect))), height);
	}
	if (previous.cx > 0 && previous != m_processingSize && m_mfgMultiplier) {
		StopMfg(false);
		m_mfg.ResetHistory();
		m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
		m_mfgInputModeKnown = false;
	}
	// Display mode/source changes can arrive without a SetVideoRect event.
	// Keep the VP intermediate at the same fixed size before the next frame.
	if (previous != m_processingSize && m_D3D11VP.IsReady() && m_bVPScaling) {
		UpdateTexures();
	}
}

HRESULT CDX11VideoProcessor::SetWindowRect(const CRect& windowRect)
{
    if (m_windowRect != windowRect || m_resizePending) {
        m_completedGeometryTick = GetTickCount64();
        InvalidateCompletedBackBuffer();
    }
    const bool sizeChanged = windowRect.Width() != m_windowRect.Width() ||
                             windowRect.Height() != m_windowRect.Height();
    // Keep resize-only bank-pressure recovery out of the Seek/Flush MFG
    // prebuffer path. Both paths may start with an empty Completed Queue,
    // but only a real window-size change may arm B08's forced batch retire.
    if (sizeChanged && m_mfgMultiplier.load(std::memory_order_relaxed) != 0 &&
        m_pFilter->m_filterState == State_Running)
        m_completedResizeRecovery.store(true, std::memory_order_relaxed);
    if (!sizeChanged) {
        // Position changes are not swap-chain resizes. When a drag returns to
        // the original size, discard the uncommitted resize request. Treat
        // cancellation of a pending size as the final geometry event so the
        // completed-frame pacing estimator also gets a clean settling window.
        if (m_resizePending) m_resizeLastChangeTick = GetTickCount64();
        m_resizePending = false;
        m_windowRect = windowRect;
        UpdateRenderRect();
        return S_OK;
    }

    // During a continuous drag, resizing DXGI buffers and tearing down NGX on
    // every WM_SIZE event is extremely expensive. Keep rendering the source to
    // the last committed buffer until the final dimensions settle. This path
    // only applies to windowed playback with MFG enabled and a running graph.
    const bool defer = m_pDXGISwapChain1 && !m_bExclusiveScreen &&
        (m_mfgMultiplier.load(std::memory_order_relaxed) != 0 ||
         CompletedFramesPending() || m_completedLast) &&
        m_pFilter->m_filterState == State_Running &&
        windowRect.Width() > 0 && windowRect.Height() > 0;
    if (defer) {
        if (!m_resizePending && m_processingSize.cx == 0 && !CompletedFramesPending()) {
            // Drain the presenter once, but retain the NGX device/session.
            // Session::Generate already handles a genuine input-size change.
            StopMfg(false);
            m_mfg.ResetHistory();
            m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
            m_mfgInputModeKnown = false;
        }
        m_resizePending = true;
        m_resizeTarget = windowRect;
        m_resizeLastChangeTick = GetTickCount64();
        // The resize must complete even if the completed-picture queue drains
        // before another video Present. Wake the presentation worker so it can
        // schedule the coalesced swap-chain commit independently of a frame.
        m_completedPresenter.Wake();
        return S_OK;
    }

    m_resizePending = false;
    if (m_mfgMultiplier.load(std::memory_order_relaxed) && m_processingSize.cx == 0) {
        StopMfg(false);
        m_mfg.ResetHistory();
        m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
        m_mfgInputModeKnown = false;
    }
    m_windowRect = windowRect;
    UpdateRenderRect();
    HRESULT hr = S_OK;
    if (m_pDXGISwapChain1 && !m_bExclusiveScreen &&
        windowRect.Width() > 0 && windowRect.Height() > 0) {
        if (m_processingSize.cx > 0) DiscardMfgPreparedFrame();
        // ResizeBuffers requires ALL direct/indirect references to the old
        // back buffers to be released, not just the current OM render target.
        // Flush deferred D3D11 releases before changing the swap-chain size.
        m_pDeviceContext->ClearState();
        m_pDeviceContext->Flush();
        hr = m_pDXGISwapChain1->ResizeBuffers(0,
            static_cast<UINT>(windowRect.Width()),
            static_cast<UINT>(windowRect.Height()), DXGI_FORMAT_UNKNOWN, EditionSwapChainFlags());
    }
    UpdateStatsByWindow();
    UpdatePostScaleTexures();
    return hr;
}

HRESULT CDX11VideoProcessor::ApplyPendingWindowResize(bool settle)
{
    if (!m_resizePending) return S_OK;
    // 140 ms after the LAST change, not after the first. The presenter
    // continues to use the old DXGI buffers at source cadence meanwhile.
    constexpr ULONGLONG settleMs = 140;
    if (settle && m_pFilter->m_filterState == State_Running &&
        GetTickCount64() - m_resizeLastChangeTick < settleMs) return S_FALSE;

    const CRect previous = m_windowRect;
    const CRect finalRect = m_resizeTarget;
    m_completedGeometryTick = GetTickCount64();
    InvalidateCompletedBackBuffer();
    m_resizePending = false;
    m_windowRect = finalRect;
    if (m_pDXGISwapChain1 && !m_bExclusiveScreen &&
        finalRect.Width() > 0 && finalRect.Height() > 0) {
        if (m_processingSize.cx > 0) DiscardMfgPreparedFrame();
        // A previously bound SRV/RTV/UAV or deferred D3D11 release can retain
        // an indirect reference to the old flip-model back buffer. Merely
        // unbinding OM targets leaves those references alive on some paths.
        m_pDeviceContext->ClearState();
        m_pDeviceContext->Flush();
        const HRESULT hr = m_pDXGISwapChain1->ResizeBuffers(0,
            static_cast<UINT>(finalRect.Width()),
            static_cast<UINT>(finalRect.Height()), DXGI_FORMAT_UNKNOWN, EditionSwapChainFlags());
        if (FAILED(hr)) {
            // Do not run the renderer with mismatched viewport/buffer sizes.
            m_windowRect = previous;
            UpdateRenderRect();
            m_resizePending = true;
            m_resizeLastChangeTick = GetTickCount64();
            DLog(L"Deferred DXGI ResizeBuffers failed: {}", HR2Str(hr));
            return hr;
        }
    }
    UpdateRenderRect();
    UpdateStatsByWindow();
    UpdatePostScaleTexures();
    if (mfg::SourceCadenceTrace().Enabled()) {
        mfg::SourceCadenceTrace().Record("window_resize_commit", GetPreciseTick(), -1, -1,
            0, 0, 0, 0, 0, false, 0, S_OK);
    }
    DLog(L"Deferred DXGI resize committed: {}x{}", finalRect.Width(), finalRect.Height());
    return S_OK;
}

