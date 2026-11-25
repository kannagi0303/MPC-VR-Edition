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
#include "Mfg/SettingsScope.h"
#include "Edition/WindowMessages.h"

std::optional<HRESULT> CMpcVideoRenderer::GetEditionBool(LPCSTR field, bool* value)
{
    if (!strcmp(field, "videoEnhancement")) {
        *value = m_videoEnhancement.load();
        return S_OK;
    }
	if (!strcmp(field, "dlssNR")) {
		*value = m_Sets.bDlssNR;
		return S_OK;
	}

	return {};
}

std::optional<HRESULT> CMpcVideoRenderer::GetEditionInt(LPCSTR field, int* value)
{
	// Independent settings channel for the MFG page. The native MPC-VR page
	// owns every other Settings_t field; it must never be asked to commit MFG.
	if (!strcmp(field, "mfgMultiplier")) {
		CAutoLock lock(&m_RendererLock);
		*value = static_cast<int>(m_Sets.iMfgMultiplier);
		return S_OK;
	}

	// The logo chosen in the settings page's list: 0 for the one found or recognised.
	if (!strcmp(field, "logoChoice")) {
		CAutoLock cRendererLock(&m_RendererLock);
		*value = m_VideoProcessor ? m_VideoProcessor->GetLogoChoice() : 0;
		return S_OK;
	}
	return {};
}

std::optional<HRESULT> CMpcVideoRenderer::SetEditionBool(LPCSTR field, bool value)
{
    // [VE-00 | 2026-11-24 16:39:20 +08:00] One live transaction; no registry write until Apply.
    if (!strcmp(field, "videoEnhancement")) {
        CAutoLock lock(&m_RendererLock);
        if (m_videoEnhancement.exchange(value) == value) return S_FALSE;
        if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) {
            // Never reuse old PTS/LUTs or an already graded backbuffer after a toggle.
            dx11->ReleaseSvdLutRawTest(true);
            dx11->InvalidateCompletedBackBuffer();
        }
        if (m_filterState == State_Paused) Redraw();
        return S_OK;
    }

	// The settings page's two buttons for the channel's logo: they act at once, as the
	// preset buttons do, with no Apply.
	if (!strcmp(field, "cmd_findLogo") && value) {
		CAutoLock cRendererLock(&m_RendererLock);
		if (m_VideoProcessor) {
			m_VideoProcessor->FindLogo();
		}
		return S_OK;
	}
	if (!strcmp(field, "cmd_forgetLogos") && value) {
		{
			CAutoLock cRendererLock(&m_RendererLock);
			if (m_VideoProcessor) {
				m_VideoProcessor->ForgetLogos();
			}
		}
		if (m_State != State_Running) {
			Redraw();
		}
		return S_OK;
	}

	// Toggling DLSS without opening the property page, so it can be bound to a
	// key or driven from a script. Same path as the checkbox: Configure()
	// applies it live, no session reload.
	if (!strcmp(field, "dlssNR")) {
		if (m_Sets.bDlssNR != value) {
			CAutoLock cRendererLock(&m_RendererLock);

			m_Sets.bDlssNR = value;
			m_VideoProcessor->Configure(m_Sets);
			SaveSettings();
			if (m_filterState != State_Running) {
				Redraw();
			}
		}
		return S_OK;
	}

	return {};
}

std::optional<HRESULT> CMpcVideoRenderer::SetEditionInt(LPCSTR field, int value)
{
	// One atomic MFG setting entry point, similar to MPCVR-Plus' IExFilterConfig
	// controls. Do not replace a whole Settings_t captured by another tab.
	// A requested multiplier is durable even when the NGX runtime is unavailable.
	if (!strcmp(field, "mfgMultiplier")) {
		if (value != 0 && (value < 2 || value > 7)) {
			return E_INVALIDARG;
		}
		// Legacy clients requesting a fixed multiplier now select Auto.
		value = value ? 7 : 0;
		{
			CAutoLock lock(&m_RendererLock);
			if (m_Sets.iMfgMultiplier != static_cast<unsigned>(value)) {
				m_Sets.iMfgMultiplier = static_cast<unsigned>(value);
				if (m_VideoProcessor) {
					m_VideoProcessor->Configure(m_Sets);
				}
			}
		}
		// SaveSettings() takes its own snapshot lock. Never call it while
		// holding the renderer lock and never require successful NGX startup.
		return SaveSettings();
	}

	// A logo of the settings page's list taken at once, as its buttons act: the i-th of
	// the last "logoList", or 0 for the one recognised by itself.
	if (!strcmp(field, "logoChoice")) {
		{
			CAutoLock cRendererLock(&m_RendererLock);
			if (m_VideoProcessor) {
				m_VideoProcessor->ChooseLogo(value);
			}
		}
		if (m_State != State_Running) {
			Redraw();
		}
		return S_OK;
	}

	return {};
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetString(LPCSTR field, LPWSTR* value, unsigned* chars)
{
	CheckPointer(value, E_POINTER);

	// One line describing the DLSS 5 NR session, or the DLSS Super Resolution one,
	// for the property page. "statsText" is the statistics as drawn on the picture,
	// for tools that log them.
	// "logoStatus": what hiding the channel's logo is doing; "logoList": the logos kept for
	// the film playing, one a line, as the settings page lists them.
	const bool bNR = !strcmp(field, "dlssStatus");
	const bool bSR = !strcmp(field, "dlssSRStatus");
	const bool bStats = !strcmp(field, "statsText");
	const bool bLogo = !strcmp(field, "logoStatus");
	const bool bLogoList = !strcmp(field, "logoList");
	const bool bProjection = !strcmp(field, "projectionProfileName");
	if (bNR || bSR || bStats || bLogo || bLogoList || bProjection) {
		CAutoLock cRendererLock(&m_RendererLock);
		std::wstring str;
		if (bProjection) {
			if (m_jvrMediaProfile && GetActive()) str = m_jvrMediaProfile->name;
		} else if (m_VideoProcessor) {
			str = bLogoList ? m_VideoProcessor->GetLogoList()
				: bLogo ? m_VideoProcessor->GetLogoStatus()
				: bStats ? m_VideoProcessor->GetStatsText()
				: bSR ? m_VideoProcessor->GetDlssSRStatus() : m_VideoProcessor->GetDlssStatus();
		}
		const size_t len = str.size();
		LPWSTR buf = (LPWSTR)CoTaskMemAlloc((len + 1) * sizeof(WCHAR));
		if (!buf) {
			return E_OUTOFMEMORY;
		}
		memcpy(buf, str.c_str(), len * sizeof(WCHAR));
		buf[len] = L'\0';
		*value = buf;
		if (chars) {
			*chars = (unsigned)len;
		}
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP_(unsigned) CMpcVideoRenderer::GetVideoProcessorUse()
{
	return m_VideoProcessor ? m_VideoProcessor->GetVideoProcessorUse() : 0;
}


void CMpcVideoRenderer::NormalizeEditionSettings()
{
	m_Sets.iJvrVertical = std::clamp(m_Sets.iJvrVertical, JVR_VERTICAL_MIN, JVR_VERTICAL_MAX);
	m_Sets.iJvrReprojection = std::clamp(m_Sets.iJvrReprojection, JVR_REPROJECTION_MIN, JVR_REPROJECTION_MAX);
	m_Sets.iJvrProfile = 0;
	m_Sets.iJvrEye = m_Sets.iJvrEye == 1 ? 1 : 0;
	m_Sets.bMfgLowMotion = false;
	m_Sets.bAdaptiveMfgDepth = false;
	m_Sets.bDepth3DPreview = false;
	// Keep both legacy callers and saved settings compatible with the
	// streamlined Off/Auto UI and the removal of the VSR Always option.
	m_Sets.iMfgMultiplier = m_Sets.iMfgMultiplier >= 2 && m_Sets.iMfgMultiplier <= 7 ? 7u : 0u;
	m_Sets.iVPSuperRes = m_Sets.iVPSuperRes == SUPERRES_Always ? SUPERRES_1440p
		: discard<int>(m_Sets.iVPSuperRes, SUPERRES_Disable, SUPERRES_Disable, SUPERRES_DLSS_SR);
	mfg::EnforceScope(m_Sets);
}

void CMpcVideoRenderer::OnEditionSettingsConfigured()
{
	if (!m_Sets.bShowFeatureBadges &&
		m_bStartupOverlayPending.exchange(false, std::memory_order_acq_rel)) {
		if (m_hWndWindow) PostMessageW(m_hWndWindow, WM_VR_STARTUP_OVERLAY, FALSE, 0);
	}

	// Picking a different key takes effect immediately; this is a no-op when
	// the key and the thread are unchanged.
	InstallToggleKeyHook();

	if (m_State == State_Paused) {
		// Re-render a pending sample even when the buffer is valid, so that the
		// change shows on it. There often is none -- the base class releases
		// each sample once drawn -- which is why this alone could not stop the
		// green frame after a rebuild of the hardware video processor; the
		// picture is now kept across that rebuild in Configure().
		if (m_pMediaSample) {
			m_bInReceive = FALSE;

			DoRenderSample(m_pMediaSample);
		}
		Redraw();
	}
}
