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

void CDX11VideoProcessor::ResetFeatureStatus()
{
	std::lock_guard guard(m_featureStatusMutex);
	m_featureStatusUntil = 0;
	m_featureStatusLastGenerated = m_mfgGeneratedPresents.load(std::memory_order_relaxed);
	m_featureStatusGeneratedTick = 0;
	m_featureStatusMfgActive = false;
}

LONG CDX11VideoProcessor::DrawFeatureStatus(ID3D11Texture2D* target)
{
	if (!target || !m_pDevice || !m_showFeatureStatus.load(std::memory_order_relaxed)) {
		return 0;
	}

	const auto state = m_pFilter->m_filterState;
	if (state == State_Stopped) return 0;
	const bool paused = state == State_Paused;
	D3D11_TEXTURE2D_DESC desc{};
	target->GetDesc(&desc);
	const CSize size(static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height));
	const ULONGLONG now = GetTickCount64();
	bool mfgActive;
	{
		std::lock_guard guard(m_featureStatusMutex);
		const auto playbackRun = m_pFilter->m_featureStatusPlaybackRun.load(std::memory_order_relaxed);
		if (m_featureStatusSize != size || m_featureStatusPlaybackRun != playbackRun || paused) {
			m_featureStatusSize = size;
			m_featureStatusPlaybackRun = playbackRun;
			m_featureStatusUntil = 0;
		}
		const auto generated = m_mfgGeneratedPresents.load(std::memory_order_relaxed);
		if (generated != m_featureStatusLastGenerated) {
			m_featureStatusLastGenerated = generated;
			m_featureStatusGeneratedTick = now;
		}
		const bool mfgPresentedRecently = m_featureStatusGeneratedTick && now - m_featureStatusGeneratedTick <= 2000;
		if (!paused) {
			m_featureStatusMfgActive = mfgPresentedRecently &&
				m_mfgEffectiveMultiplier.load(std::memory_order_relaxed) >= 2;
		}
		// A paused picture can keep the current MFG label even though no new
		// generated frames are presented. Preroll may already have warmed it up.
		mfgActive = (m_featureStatusMfgActive || (paused && m_mfg.InputSize().cx > 0)) &&
			m_mfgMultiplier.load(std::memory_order_relaxed) >= 2 &&
			!m_mfgPresentFailed.load(std::memory_order_relaxed) && !m_mfg.Failed() && !MfgSourceMeetsDisplayRate();
		if (paused) m_featureStatusMfgActive = mfgActive;
		if (m_featureStatusUntil && now >= m_featureStatusUntil) return 0;
	}

	const bool srActive = m_iVPSuperRes == SUPERRES_DLSS_SR && m_dlssSR.Active();
	const auto processingRect = ProcessingRect();
	const auto processingWidth = processingRect.Width();
	const auto processingHeight = processingRect.Height();
	const bool upscale = m_srcRectWidth > 0 && m_srcRectHeight > 0 &&
		processingWidth > 0 && processingHeight > 0 &&
		m_srcRectWidth < static_cast<UINT>(processingWidth) &&
		m_srcRectHeight < static_cast<UINT>(processingHeight);
	const bool vsrActive = m_iVPSuperRes != SUPERRES_DLSS_SR &&
		m_VendorId == PCIV_NVIDIA && m_bVPScaling && m_bVPUseSuperRes && upscale;
	const auto* text = CMpcVideoRenderer::FeatureStatusText(srActive, vsrActive, mfgActive);
	if (!*text) return 0;

	constexpr LONG margin = 12, padX = 9, padY = 5;
	if (FAILED(m_Font3D.CreateFontBitmap(L"Consolas", m_StatsFontH, 0))) {
		return 0;
	}
	SIZE textSize{};
	m_Font3D.GetTextExtent(text, &textSize);
	const LONG width = textSize.cx + 2 * padX;
	const LONG height = textSize.cy + 2 * padY;
	if (width + 2 * margin > size.cx || height + 2 * margin > size.cy) {
		return 0;
	}

	const LONG x = margin;
	const LONG y = margin;
	CComPtr<ID3D11RenderTargetView> rtv;
	const HRESULT hr = m_pDevice->CreateRenderTargetView(target, nullptr, &rtv);
	if (FAILED(hr)) {
		return 0;
	}
	const RECT panel{x, y, x + width, y + height};
	if (SUCCEEDED(m_FeatureStatusBackground.Set(panel, size, D3DCOLOR_ARGB(170, 0, 0, 0)))) {
		m_FeatureStatusBackground.Draw(rtv, size);
	}
	const HRESULT drawn = m_Font3D.Draw2DText(rtv, size, x + padX, y + padY, D3DCOLOR_XRGB(255, 255, 255), text);
	if (!paused && SUCCEEDED(drawn)) {
		std::lock_guard guard(m_featureStatusMutex);
		if (!m_featureStatusUntil) {
			m_featureStatusUntil = now + 5000;
		}
	}
	return SUCCEEDED(drawn) ? height + margin : 0;
}

