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
#include <algorithm>
#include "DLSS/DecodedFrameLookahead.h"
#include "DLSS/ZipDepthLookahead.h"
#include <atomic>
#include <optional>
#include <evr.h> // for MR_VIDEO_ACCELERATION_SERVICE, because the <mfapi.h> does not contain it
#include <Mferror.h>
#include "Helper.h"
#include "JvrProjection.h"
#include "PropPage.h"
#include "SettingsPage/SettingsPage.h"
#include "Mfg/SettingsScope.h"
#include "Edition/RendererSettings.h"
#include "Edition/SettingsRegistry.h"
#include "Edition/WindowMessages.h"
#include "Mfg/AvcPresentationClock.h"
#include "VideoRendererInputPin.h"
#include "Version.h"
#include "VideoRenderer.h"
#include "SubPic/XySubPicProvider.h"
#include "SubPic/XySubPicQueueImpl.h"

#define WM_SWITCH_FULLSCREEN (WM_APP + 0x1000)
#define OPT_REGKEY_VIDEORENDERER           edition::RendererRegistryKey
#define OPT_UseD3D11                       L"UseD3D11"
#define OPT_ShowStatistics                 L"ShowStatistics"
#define OPT_ResizeStatistics               L"ResizeStatistics"
#define OPT_TextureFormat                  L"TextureFormat"
#define OPT_VPEnableNV12                   L"VPEnableNV12"
#define OPT_VPEnableP01x                   L"VPEnableP01x"
#define OPT_VPEnableYUY2                   L"VPEnableYUY2"
#define OPT_VPEnableOther                  L"VPEnableOther"
#define OPT_VPDeinterlacing                L"VPDeinterlacing"
#define OPT_DoubleFrateDeint               L"DoubleFramerateDeinterlace"
#define OPT_VPScaling                      L"VPScaling"
#define OPT_VPSuperResolution              L"VPSuperResolution"
#define OPT_VPRTXVideoHDR                  L"VPRTXVideoHDR"
#define OPT_ChromaUpsampling               L"ChromaUpsampling"
#define OPT_Upscaling                      L"Upscaling"
#define OPT_Downscaling                    L"Downscaling"
#define OPT_InterpolateAt50pct             L"InterpolateAt50pct"
#define OPT_Dither                         L"Dither"
#define OPT_DeintBlend                     L"DeinterlaceBlend"
#define OPT_SwapEffect                     L"SwapEffect"
#define OPT_ExclusiveFullscreen            L"ExclusiveFullscreen"
#define OPT_VBlankBeforePresent            L"VBlankBeforePresent"
#define OPT_AdjustPresentTime              L"AdjustPresentationTime"
#define OPT_ReinitByDisplay                L"ReinitWhenChangingDisplay"
#define OPT_HdrPreferDoVi                  L"HdrPreferDoVi"
#define OPT_HdrPassthrough                 L"HdrPassthrough"
#define OPT_HdrLocaLToneMapping            L"HdrLocalToneMapping"
#define OPT_HdrLocaLToneMappingType        L"HdrLocalToneMappingType"
#define OPT_HdrLocalMeasurePeak            L"HdrLocalMeasurePeak"
#define OPT_HdrDoviToneMappingType         L"HdrDoviToneMappingType"
#define OPT_HdrDisplayNits                 L"HdrDisplayNits"
#define OPT_HdrToggleDisplay               L"HdrToggleDisplay"
#define OPT_HdrOsdBrightness               L"HdrOsdBrightness"
#define OPT_ConvertToSdr                   L"ConvertToSdr"
#define OPT_UseD3DFullscreen               L"UseD3DFullscreen"
#define OPT_DisplayNits                    L"DisplayNits"
#define OPT_SdrToneMappingType             L"SdrToneMappingType"
// registry only, not saved
#define OPT_SdrPeakWindowMs                L"SdrPeakWindowMs"
#define OPT_SdrPeakFloorNits               L"SdrPeakFloorNits"
#define OPT_SdrPeakSceneCuts               L"SdrPeakSceneCuts"

static std::atomic_int g_nInstance = 0;


LPCWSTR g_pszOldParentWndProc = L"OldParentWndProc";
LPCWSTR g_pszThis = L"This";

static void RemoveParentWndProc(HWND hWnd)
{
	DLog(L"RemoveParentWndProc()");
	auto pfnOldProc = GetPropW(hWnd, g_pszOldParentWndProc);
	if (pfnOldProc) {
		auto lpPreviousProc = SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
		if (lpPreviousProc != 0) {
			RemovePropW(hWnd, g_pszOldParentWndProc);
			RemovePropW(hWnd, g_pszThis);
		} else {
			ASSERT(false);
		}
	}
}

static LRESULT CALLBACK ParentWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	auto pfnOldProc = reinterpret_cast<WNDPROC>(GetPropW(hWnd, g_pszOldParentWndProc));
	auto pThis = static_cast<CMpcVideoRenderer*>(GetPropW(hWnd, g_pszThis));

	switch (Msg) {
		case WM_DESTROY:
			if (pfnOldProc) {
				auto lpPreviousProc = SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
				if (lpPreviousProc != 0) {
					RemovePropW(hWnd, g_pszOldParentWndProc);
					RemovePropW(hWnd, g_pszThis);
				} else {
					ASSERT(false);
				}
			}
			break;
		case WM_DISPLAYCHANGE:
			DLog(L"ParentWndProc() - WM_DISPLAYCHANGE");
			pThis->OnDisplayModeChange(true);
			break;
		case WM_MOVE:
			if (pThis->m_bExclusiveScreen) {
				// I don't know why, but without this, the filter freezes when switching from fullscreen to window in DX9 mode.
				if (pfnOldProc) {
					SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pfnOldProc));
					SetWindowLongPtrW(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ParentWndProc));
				}
			} else {
				pThis->OnWindowMove();
			}
			break;
		case WM_NCACTIVATE:
			if (!wParam && pThis->m_bExclusiveScreen && !pThis->m_bIsD3DFullscreen) {
				return 0;
			}
			break;
		case WM_RBUTTONUP:
			if (pThis->m_bExclusiveScreen) {
				// block context menu in exclusive fullscreen
				return 0;
			}
			break;
/*
		case WM_SYSCOMMAND:
			if (pThis->m_bExclusiveScreen && wParam == SC_MINIMIZE) {
				// block minimize in exclusive fullscreen
				return 0;
			}
			break;
*/
	}

	if (!pfnOldProc || reinterpret_cast<LONG_PTR>(pfnOldProc) == reinterpret_cast<LONG_PTR>(ParentWndProc)) {
		ASSERT(false);
		return 0;
	}

	return CallWindowProcW(pfnOldProc, hWnd, Msg, wParam, lParam);
}

//
// CMpcVideoRenderer
//

void CMpcVideoRenderer::ReadSettings(CRegKey& key, Settings_t& sets)
{
	DWORD dw;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_UseD3D11, dw)) {
		sets.bUseD3D11 = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ShowStatistics, dw)) {
		sets.bShowStats = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ResizeStatistics, dw)) {
		sets.iResizeStats = discard<int>(dw, 0, 0, 1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_TextureFormat, dw)) {
		switch (dw) {
		case TEXFMT_AUTOINT:
		case TEXFMT_8INT:
		case TEXFMT_10INT:
		case TEXFMT_16FLOAT:
			sets.iTexFormat = dw;
			break;
		default:
			sets.iTexFormat = TEXFMT_AUTOINT;
		}
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableNV12, dw)) {
		sets.VPFmts.bNV12 = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableP01x, dw)) {
		sets.VPFmts.bP01x = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableYUY2, dw)) {
		sets.VPFmts.bYUY2 = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPEnableOther, dw)) {
		sets.VPFmts.bOther = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPDeinterlacing, dw)) {
		sets.iVPDeinterlacing = discard<int>(dw, DEINT_Enable, DEINT_Disable, DEINT_HackFutureFrames);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DoubleFrateDeint, dw)) {
		sets.bDeintDouble = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPScaling, dw)) {
		sets.bVPScaling = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPSuperResolution, dw)) {
		sets.iVPSuperRes = discard<int>(dw, SUPERRES_Disable, 0, SUPERRES_COUNT-1);
	}
#ifdef _WIN64
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPRTXVideoHDR, dw)) {
		sets.bVPRTXVideoHDR = !!dw;
	}
#endif
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ChromaUpsampling, dw)) {
		sets.iChromaScaling = discard<int>(dw, CHROMA_Bilinear, 0, CHROMA_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Upscaling, dw)) {
		sets.iUpscaling = discard<int>(dw, UPSCALE_CatmullRom, 0, UPSCALE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Downscaling, dw)) {
		sets.iDownscaling = discard<int>(dw, DOWNSCALE_Hamming, 0, DOWNSCALE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_InterpolateAt50pct, dw)) {
		sets.bInterpolateAt50pct = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Dither, dw)) {
		sets.bUseDither = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DeintBlend, dw)) {
		sets.bDeintBlend = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SwapEffect, dw)) {
		sets.iSwapEffect = discard<int>(dw, SWAPEFFECT_Flip, SWAPEFFECT_Discard, SWAPEFFECT_Flip);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ExclusiveFullscreen, dw)) {
		sets.bExclusiveFS = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VBlankBeforePresent, dw)) {
		sets.bVBlankBeforePresent = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_AdjustPresentTime, dw)) {
		sets.bAdjustPresentTime = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ReinitByDisplay, dw)) {
		sets.bReinitByDisplay = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrPreferDoVi, dw)) {
		sets.bHdrPreferDoVi = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrPassthrough, dw)) {
		sets.bHdrPassthrough = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrLocaLToneMapping, dw)) {
		sets.bHdrLocalToneMapping = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrLocaLToneMappingType, dw)) {
		sets.iHdrLocalToneMappingType = discard<int>(dw, 1, 1, 7);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrLocalMeasurePeak, dw)) {
		sets.bHdrLocalMeasurePeak = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrDoviToneMappingType, dw)) {
		sets.iHdrDoviToneMappingType = dw == 0 ? 0 : 6;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrDisplayNits, dw)) {
		sets.iHdrDisplayMaxNits = discard<int>(dw, HDR_NITS_DEF, HDR_NITS_MIN, HDR_NITS_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrToggleDisplay, dw)) {
		sets.iHdrToggleDisplay = discard<int>(dw, HDRTD_On, HDRTD_Disabled, HDRTD_OnOff);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HdrOsdBrightness, dw)) {
		sets.iHdrOsdBrightness = discard<int>(dw, 0, 0, 2);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ConvertToSdr, dw)) {
		sets.bConvertToSdr = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrToneMappingType, dw)) {
		sets.iSdrToneMappingType = discard<int>(dw, SDRTM_Metadata, SDRTM_Fixed, SDRTM_COUNT - 1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DisplayNits, dw)) {
		sets.iSDRDisplayNits = discard<int>(dw, SDR_NITS_DEF, SDR_NITS_MIN, SDR_NITS_MAX);
	}
	edition::ReadSettings(key, sets);
}

void CMpcVideoRenderer::WriteSettings(CRegKey& key, const Settings_t& sets)
{
	key.SetDWORDValue(OPT_UseD3D11,            sets.bUseD3D11);
	key.SetDWORDValue(OPT_ShowStatistics,      sets.bShowStats);
	key.SetDWORDValue(OPT_ResizeStatistics,    sets.iResizeStats);
	key.SetDWORDValue(OPT_TextureFormat,       sets.iTexFormat);
	key.SetDWORDValue(OPT_VPEnableNV12,        sets.VPFmts.bNV12);
	key.SetDWORDValue(OPT_VPEnableP01x,        sets.VPFmts.bP01x);
	key.SetDWORDValue(OPT_VPEnableYUY2,        sets.VPFmts.bYUY2);
	key.SetDWORDValue(OPT_VPEnableOther,       sets.VPFmts.bOther);
	key.SetDWORDValue(OPT_VPDeinterlacing,     sets.iVPDeinterlacing);
	key.SetDWORDValue(OPT_DoubleFrateDeint,    sets.bDeintDouble);
	key.SetDWORDValue(OPT_VPScaling,           sets.bVPScaling);
	key.SetDWORDValue(OPT_VPSuperResolution,   sets.iVPSuperRes);
#ifdef _WIN64
	key.SetDWORDValue(OPT_VPRTXVideoHDR,       sets.bVPRTXVideoHDR);
#endif
	key.SetDWORDValue(OPT_ChromaUpsampling,    sets.iChromaScaling);
	key.SetDWORDValue(OPT_Upscaling,           sets.iUpscaling);
	key.SetDWORDValue(OPT_Downscaling,         sets.iDownscaling);
	key.SetDWORDValue(OPT_InterpolateAt50pct,  sets.bInterpolateAt50pct);
	key.SetDWORDValue(OPT_Dither,              sets.bUseDither);
	key.SetDWORDValue(OPT_DeintBlend,          sets.bDeintBlend);
	key.SetDWORDValue(OPT_SwapEffect,          sets.iSwapEffect);
	key.SetDWORDValue(OPT_ExclusiveFullscreen, sets.bExclusiveFS);
	key.SetDWORDValue(OPT_VBlankBeforePresent, sets.bVBlankBeforePresent);
	key.SetDWORDValue(OPT_AdjustPresentTime,   sets.bAdjustPresentTime);
	key.SetDWORDValue(OPT_ReinitByDisplay,     sets.bReinitByDisplay);
	key.SetDWORDValue(OPT_HdrPreferDoVi,       sets.bHdrPreferDoVi);
	key.SetDWORDValue(OPT_HdrPassthrough,      sets.bHdrPassthrough);
	key.SetDWORDValue(OPT_HdrLocaLToneMapping, sets.bHdrLocalToneMapping);
	key.SetDWORDValue(OPT_HdrLocaLToneMappingType, sets.iHdrLocalToneMappingType);
	key.SetDWORDValue(OPT_HdrLocalMeasurePeak, sets.bHdrLocalMeasurePeak);
	key.SetDWORDValue(OPT_HdrDoviToneMappingType, sets.iHdrDoviToneMappingType);
	key.SetDWORDValue(OPT_HdrDisplayNits,      sets.iHdrDisplayMaxNits);
	key.SetDWORDValue(OPT_HdrToggleDisplay,    sets.iHdrToggleDisplay);
	key.SetDWORDValue(OPT_HdrOsdBrightness,    sets.iHdrOsdBrightness);
	key.SetDWORDValue(OPT_ConvertToSdr,        sets.bConvertToSdr);
	key.SetDWORDValue(OPT_SdrToneMappingType,  sets.iSdrToneMappingType);
	key.SetDWORDValue(OPT_DisplayNits,         sets.iSDRDisplayNits);
	edition::WriteSettings(key, sets);
}

CMpcVideoRenderer::CMpcVideoRenderer(LPUNKNOWN pUnk, HRESULT* phr)
	: CBaseVideoRenderer2(__uuidof(this), L"MPC-VR-Edition", pUnk, phr)
{
	DLog(L"CMpcVideoRenderer::CMpcVideoRenderer()");

	auto nPrevInstance = g_nInstance++; // always increment g_nInstance in the constructor
	if (nPrevInstance > 0) {
		*phr = E_ABORT;
		DLog(L"Previous copy of CMpcVideoRenderer found! Initialization aborted.");
		return;
	}

	DLog(L"Windows {}", GetWindowsVersion());
	DLog(GetNameAndVersion());

	ASSERT(S_OK == *phr);
	m_pInputPin = new CVideoRendererInputPin(this, phr, L"In", this);
	ASSERT(S_OK == *phr);

	// read settings

	CRegKey key;
	if (ERROR_SUCCESS == key.Open(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER, KEY_READ)) {
		ReadSettings(key, m_Sets);
        // [VE-00 | 2026-11-24 16:39:20 +08:00] Global setting; legacy settings only migrate once.
        DWORD ve = 0;
        if (key.QueryDWORDValue(L"VideoEnhancement", ve) == ERROR_SUCCESS && ve <= 1) {
            m_videoEnhancement.store(ve == 1);
        } else {
            DWORD mode = 0, strength = 200, preferred = 0, skin = 0;
            if (key.QueryDWORDValue(L"SvdLutMode", mode) != ERROR_SUCCESS || mode > 3) mode = 0;
            if (key.QueryDWORDValue(L"SvdLutBStrength", strength) != ERROR_SUCCESS || strength > 200) strength = 200;
            if (key.QueryDWORDValue(L"PreferredColors", preferred) != ERROR_SUCCESS) preferred = 0;
            if (key.QueryDWORDValue(L"SkinTones", skin) != ERROR_SUCCESS) skin = 0;
            const bool legacyGrade = mode == 0 ? m_Sets.GetSvdLutStrength() != 0 :
                (mode == 1 || mode == 2) && strength != 0;
            m_videoEnhancement.store(legacyGrade || preferred == 1 || skin == 1);
        }
		// Upstream's registry-only tuning of the Measured method: read here and nowhere
		// else, never written, never in a preset.
		DWORD dw;
		if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrPeakWindowMs, dw)) {
			m_Sets.iSdrPeakWindowMs = discard<int>(dw, SDR_PEAK_WINDOW_DEF, SDR_PEAK_WINDOW_MIN, SDR_PEAK_WINDOW_MAX);
		}
		if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrPeakFloorNits, dw)) {
			m_Sets.iSdrPeakFloorNits = discard<int>(dw, SDR_PEAK_FLOOR_DEF, SDR_PEAK_FLOOR_MIN, SDR_PEAK_FLOOR_MAX);
		}
		if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SdrPeakSceneCuts, dw)) {
			m_Sets.bSdrPeakSceneCuts = !!dw;
		}
	}
	ReadPresetKeys();

	if (!IsWindows10OrGreater()) {
		m_Sets.bHdrPassthrough = false;
		m_Sets.iHdrToggleDisplay = HDRTD_Disabled;
	}

	mfg::EnforceScope(m_Sets);
	HRESULT hr = S_FALSE;


	if (!m_VideoProcessor && m_Sets.bUseD3D11 && IsWindows7SP1OrGreater()) {
		m_VideoProcessor.reset(new CDX11VideoProcessor(this, m_Sets, hr));
		if (SUCCEEDED(hr)) {
			hr = m_VideoProcessor->Init(m_hWnd, false);
		}

		if (FAILED(hr)) {
			m_VideoProcessor.reset();
		}
		DLogIf(S_OK == hr, L"Direct3D11 initialization successfully!");
	}

	if (!m_VideoProcessor) {
		m_VideoProcessor.reset(new CDX9VideoProcessor(this, m_Sets, hr));
		if (SUCCEEDED(hr)) {
			hr = m_VideoProcessor->Init(::GetForegroundWindow(), false);
		}

		DLogIf(S_OK == hr, L"Direct3D9 initialization successfully!");
	}

	*phr = hr;

	return;
}

CMpcVideoRenderer::~CMpcVideoRenderer()
{
	ShutdownEditionSource();
	DLog(L"CMpcVideoRenderer::~CMpcVideoRenderer()");

	ReleaseEditionUiState();

	UnregisterClassW(RendererWindowClassName, g_hInst);

	if (m_hWndParentMain) {
		RemoveParentWndProc(m_hWndParentMain);
	}

	if (m_bExclusiveScreen && !m_bIsD3DFullscreen && m_hWndParentMain) {
		PostMessageW(m_hWndParentMain, WM_SWITCH_FULLSCREEN, 0, 0);
	}

	m_VideoProcessor.reset();

	if (m_hWndWindow) {
		::SendMessageW(m_hWndWindow, WM_CLOSE, 0, 0);
	}

	g_nInstance--; // always decrement g_nInstance in the destructor
}

void CMpcVideoRenderer::NewSegment(REFERENCE_TIME startTime, double rate)
{
	DLog(L"CMpcVideoRenderer::NewSegment()");

	m_rtStartTime = startTime;
	BeginEditionSegment(rate);
}

HRESULT CMpcVideoRenderer::BeginFlush()
{
	DLog(L"CMpcVideoRenderer::BeginFlush()");
	m_bFlushing = true;
	BeginEditionFlush();
    const HRESULT hr = __super::BeginFlush();
    return hr;
}

HRESULT CMpcVideoRenderer::EndFlush()
{
	DLog(L"CMpcVideoRenderer::EndFlush()");
	m_VideoProcessor->Flush();
	HRESULT hr = __super::EndFlush();
	m_bFlushing = false;
	return hr;
}

long CMpcVideoRenderer::CalcImageSize(CMediaType& mt, bool redefine_mt)
{
	BITMAPINFOHEADER* pBIH = GetBIHfromVIHs(&mt);
	if (!pBIH) {
		ASSERT(FALSE); // excessive checking
		return 0;
	}

	if (redefine_mt) {
		CAutoLock cRendererLock(&m_RendererLock);

		CSize Size(pBIH->biWidth, pBIH->biHeight);

		BOOL ret = m_VideoProcessor->GetAlignmentSize(mt, Size);

		if (ret && (Size.cx != pBIH->biWidth || Size.cy != pBIH->biHeight)) {
			BYTE* pbFormat = mt.ReallocFormatBuffer(VR_EXRADATA_POS + sizeof(VR_Extradata));
			if (pbFormat) {
				// update pointer after realoc
				pBIH = GetBIHfromVIHs(&mt);
				// copy data to VR_Extradata
				VR_Extradata* vrextra = reinterpret_cast<VR_Extradata*>(pbFormat + VR_EXRADATA_POS);
				vrextra->QueryWidth  = Size.cx;
				vrextra->QueryHeight = Size.cy;
				vrextra->FrameWidth  = pBIH->biWidth;
				vrextra->FrameHeight = pBIH->biHeight;
				vrextra->Compression = pBIH->biCompression;
			}

			// new media type must have non-empty rcSource
			RECT& rcSource = ((VIDEOINFOHEADER*)mt.pbFormat)->rcSource;
			if (IsRectEmpty(&rcSource)) {
				rcSource = { 0, 0, pBIH->biWidth, abs(pBIH->biHeight) };
			}
			RECT& rcTarget = ((VIDEOINFOHEADER*)mt.pbFormat)->rcTarget;
			if (IsRectEmpty(&rcTarget)) {
				// CoreAVC Video Decoder does not work correctly with empty rcTarget
				rcTarget = rcSource;
			}

			DLog(L"CMpcVideoRenderer::CalcImageSize() buffer size changed from {}x{} to {}x{}", pBIH->biWidth, pBIH->biHeight, Size.cx, Size.cy);
			// overwrite buffer size
			pBIH->biWidth  = Size.cx;
			pBIH->biHeight = Size.cy;
			pBIH->biSizeImage = DIBSIZE(*pBIH);
		}
	}

	return pBIH->biSizeImage ? pBIH->biSizeImage : DIBSIZE(*pBIH);
}

// CBaseRenderer

HRESULT CMpcVideoRenderer::CheckMediaType(const CMediaType* pmt)
{
	CheckPointer(pmt, E_POINTER);
	CheckPointer(pmt->pbFormat, E_POINTER);

	if (pmt->majortype == MEDIATYPE_Video && (pmt->formattype == FORMAT_VideoInfo2 || pmt->formattype == FORMAT_VideoInfo)) {
		for (const auto& sudPinType : sudPinTypesIn) {
			if (pmt->subtype == *sudPinType.clsMinorType) {
				CAutoLock cRendererLock(&m_RendererLock);

				if (!m_VideoProcessor->VerifyMediaType(pmt)) {
					return VFW_E_UNSUPPORTED_VIDEO;
				}

				return S_OK;
			}
		}
	}

	return E_FAIL;
}

HRESULT CMpcVideoRenderer::SetMediaType(const CMediaType *pmt)
{
	DLog(L"CMpcVideoRenderer::SetMediaType()\n{}", MediaType2Str(pmt));

	CheckPointer(pmt, E_POINTER);
	CheckPointer(pmt->pbFormat, E_POINTER);

	CAutoLock cVideoLock(&m_InterfaceLock);
	CAutoLock cRendererLock(&m_RendererLock);

	CSize framesize, aspect;
	m_VideoProcessor->GetVideoSize(framesize.cx, framesize.cy);
	m_VideoProcessor->GetAspectRatio(aspect.cx, aspect.cy);

	CMediaType mt(*pmt);

	m_bSetNewMediaTypeToInputPin = false;

	auto inputPin = static_cast<CVideoRendererInputPin*>(m_pInputPin);
	inputPin->ClearNewMediaType();
	if (!inputPin->FrameInVideoMem()) {
		CMediaType mtNew(*pmt);
		long ret = CalcImageSize(mtNew, true);

		if (mtNew != mt) {
			if (S_OK == m_pInputPin->GetConnected()->QueryAccept(&mtNew)) {
				DLog(L"CMpcVideoRenderer::SetMediaType() : upstream filter accepted new media type. QueryAccept return S_OK");
				inputPin->SetNewMediaType(mtNew);
				m_bSetNewMediaTypeToInputPin = true;
			}
		}
	}

	if (mt != m_inputMT && !m_VideoProcessor->InitMediaType(&mt)) {
		return VFW_E_UNSUPPORTED_VIDEO;
	}

	if (!m_videoRect.IsRectNull()) {
		m_VideoProcessor->SetVideoRect(m_videoRect);
	}

	CSize framesizeNew, aspectNew;
	m_VideoProcessor->GetVideoSize(framesizeNew.cx, framesizeNew.cy);
	m_VideoProcessor->GetAspectRatio(aspectNew.cx, aspectNew.cy);

	if (framesize.cx && aspect.cx && m_pSink) {
		if (aspectNew != aspect || framesizeNew != framesize
				|| aspectNew != m_videoAspectRatio || framesizeNew != m_videoSize) {
			m_pSink->Notify(EC_VIDEO_SIZE_CHANGED, MAKELPARAM(framesizeNew.cx, framesizeNew.cy), 0);
		}
	}

	m_videoSize = framesizeNew;
	m_videoAspectRatio = aspectNew;

	return S_OK;
}

// The film's name is read here, once: the graph is being built on this thread and answers
// at once. Asked again from InitMediaType, it was asked on the streaming thread too -- a
// Dolby Vision film sets its media type again on its first picture -- with the renderer's
// locks held, and the graph then waited for the thread starting it, which was waiting for
// those locks: the player hung as the film opened, whenever the logo was to be hidden.
HRESULT CMpcVideoRenderer::DoRenderSample(IMediaSample* pSample)
{
	CheckPointer(pSample, E_POINTER);

	if (m_bSetNewMediaTypeToInputPin) {
		auto inputPin = static_cast<CVideoRendererInputPin*>(m_pInputPin);
		inputPin->ClearNewMediaType();
		m_bSetNewMediaTypeToInputPin = false;
	}

	HRESULT hr = m_VideoProcessor->ProcessSample(pSample);
	m_trRenderHeld = (int)std::min<REFERENCE_TIME>(m_VideoProcessor->TakeHeldTime(), INT_MAX);

	if (SUCCEEDED(hr)) {
		m_bValidBuffer = true;
		// Preroll prepares the source and GPU resources, but does not mean a
		// picture was presented. The overlay stays until OnVideoPresented().
	}

	if (m_Stepping && !(--m_Stepping)) {
		this->NotifyEvent(EC_STEP_COMPLETE, 0, 0);
	}

	return hr;
}

// Edition owns source timing, buffering, and delivery; locks are taken inside.
HRESULT CMpcVideoRenderer::Receive(IMediaSample* sample)
{
	return ReceiveEditionSample(sample);
}

void CMpcVideoRenderer::UpdateDisplayInfo()
{
	const HMONITOR hMonPrimary = MonitorFromPoint(CPoint(0, 0), MONITOR_DEFAULTTOPRIMARY);

	MONITORINFOEXW mi = { sizeof(mi) };
	GetMonitorInfoW(m_hMon, (MONITORINFO*)&mi);

	bool ret = GetDisplayConfig(mi.szDevice, m_DisplayConfig);
	if (m_hMon == hMonPrimary) {
		m_bPrimaryDisplay = true;
	} else {
		m_bPrimaryDisplay = false;
	}

	m_VideoProcessor->SetDisplayInfo(m_DisplayConfig, m_bPrimaryDisplay, m_bExclusiveScreen);
}

void CMpcVideoRenderer::OnDisplayModeChange(const bool bReset/* = false*/)
{
	if (m_bDisplayModeChanging) {
		return;
	}

	m_bDisplayModeChanging = true;

	if (bReset && !m_VideoProcessor->IsInit()) {
		m_VideoProcessor->Reset(true);
	}

	m_hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
	UpdateDisplayInfo();

	m_bDisplayModeChanging = false;
}

void CMpcVideoRenderer::OnWindowMove()
{
	if (GetActive()) {
		const HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
		if (hMon != m_hMon) {
			if (m_Sets.bReinitByDisplay) {
				CAutoLock cRendererLock(&m_RendererLock);

				Init(true);
			}
			else if (m_VideoProcessor->Type() == VP_DX11) {
				CAutoLock cRendererLock(&m_RendererLock);

				m_VideoProcessor->Reset(false);
			}

			m_hMon = hMon;
			UpdateDisplayInfo();
			m_VideoProcessor->UpdateStatsByDisplay();
		}
	}
}

STDMETHODIMP CMpcVideoRenderer::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
	CheckPointer(ppv, E_POINTER);

	IFQIRETURN(IKsPropertySet)
	IFQIRETURN(IMFGetService)
	IFQIRETURN(IBasicVideo)
	IFQIRETURN(IBasicVideo2)
	IFQIRETURN(IVideoWindow)
	IFQIRETURN(ISpecifyPropertyPages)
	IFQIRETURN(IVideoRenderer)
	IFQIRETURN(IExFilterConfig)
	if (riid == __uuidof(ISubRender) && m_VideoProcessor && m_VideoProcessor->Type() == 9) {
		return GetInterface((ISubRender*)this, ppv);
	}
	if (riid == __uuidof(ISubRender11) && m_VideoProcessor && m_VideoProcessor->Type() == 11) {
		return GetInterface((ISubRender11*)this, ppv);
	}
	if (riid == __uuidof(ID3DFullscreenControl) && m_bEnableFullscreenControl) {
		return GetInterface((ID3DFullscreenControl*)this, ppv);
	}
	if (riid == __uuidof(ISubRenderConsumer2)) {
		return GetInterface((ISubRenderConsumer2*)this, ppv);
	}
	if (riid == __uuidof(ISubRenderConsumer)) {
		return GetInterface((ISubRenderConsumer*)this, ppv);
	}
	if (riid == __uuidof(ISubRenderOptions)) {
		return GetInterface((ISubRenderOptions*)this, ppv);
	}
	return __super::NonDelegatingQueryInterface(riid, ppv);
}

// IMediaFilter
STDMETHODIMP CMpcVideoRenderer::Run(REFERENCE_TIME rtStart)
{
	DLog(L"CMpcVideoRenderer::Run()");

	if (m_State == State_Running) {
		return NOERROR;
	}

	CAutoLock cVideoLock(&m_InterfaceLock);
	BeginEditionRun();

	const auto hr = CBaseVideoRenderer2::Run(rtStart);
	CompleteEditionRun(hr);
	return hr;
}

STDMETHODIMP CMpcVideoRenderer::Pause()
{
	DLog(L"CMpcVideoRenderer::Pause()");

	const auto previous = BeginEditionPause();

	const HRESULT hr = CBaseVideoRenderer2::Pause();
	CompleteEditionPause(hr, previous);
	return hr;
}

STDMETHODIMP CMpcVideoRenderer::Stop()
{
	DLog(L"CMpcVideoRenderer::Stop()");

	BeginEditionStop();
    const HRESULT hr = CBaseVideoRenderer2::Stop();
	return hr;
}

#if _DEBUG
std::wstring PropSetAndIdToString(REFGUID PropSet, ULONG Id)
{
#define UNPACK_VALUE(VALUE) case VALUE: str += L#VALUE; break;
	std::wstring str;
	if (PropSet == AM_KSPROPSETID_CopyProt) {
		str.assign(L"AM_KSPROPSETID_CopyProt, ");
		switch (Id) {
			UNPACK_VALUE(AM_PROPERTY_COPY_MACROVISION);
			UNPACK_VALUE(AM_PROPERTY_COPY_ANALOG_COMPONENT);
			UNPACK_VALUE(AM_PROPERTY_COPY_DIGITAL_CP);
		default:
			str += std::to_wstring(Id);
		};
	}
	else if (PropSet == AM_KSPROPSETID_FrameStep) {
		str.assign(L"AM_KSPROPSETID_FrameStep, ");
		switch (Id) {
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_STEP);
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_CANCEL);
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_CANSTEP);
			UNPACK_VALUE(AM_PROPERTY_FRAMESTEP_CANSTEPMULTIPLE);
		default:
			str += std::to_wstring(Id);
		};
	}
	else {
		str.assign(GUIDtoWString(PropSet) + L", " + std::to_wstring(Id));
	}
	return str;
#undef UNPACK_VALUE
}

#endif

// IKsPropertySet
STDMETHODIMP CMpcVideoRenderer::Set(REFGUID PropSet, ULONG Id, LPVOID pInstanceData, ULONG InstanceLength, LPVOID pPropertyData, ULONG DataLength)
{
	DLog(L"IKsPropertySet::Set({}, {}, {}, {}, {})", PropSetAndIdToString(PropSet, Id), pInstanceData, InstanceLength, pPropertyData, DataLength);

	if (PropSet == AM_KSPROPSETID_CopyProt) {
		if (Id == AM_PROPERTY_COPY_MACROVISION || Id == AM_PROPERTY_COPY_DIGITAL_CP) {
			DLogIf(Id == AM_PROPERTY_COPY_MACROVISION, L"No Macrovision please");
			DLogIf(Id == AM_PROPERTY_COPY_DIGITAL_CP, L"No Digital CP please");
			return S_OK;
		}
	}
	else if (PropSet == AM_KSPROPSETID_FrameStep) {
		if (Id == AM_PROPERTY_FRAMESTEP_STEP) {
			m_Stepping = 1;
			return S_OK;
		}
		if (Id == AM_PROPERTY_FRAMESTEP_CANSTEP || Id == AM_PROPERTY_FRAMESTEP_CANSTEPMULTIPLE) {
			return S_OK;
		}
	}
	else {
		return E_PROP_SET_UNSUPPORTED;
	}

	return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP CMpcVideoRenderer::Get(REFGUID PropSet, ULONG Id, LPVOID pInstanceData, ULONG InstanceLength, LPVOID pPropertyData, ULONG DataLength, ULONG* pBytesReturned)
{
	DLog(L"IKsPropertySet::Get({}, {}, {}, {}, {}, ...)", PropSetAndIdToString(PropSet, Id), pInstanceData, InstanceLength, pPropertyData, DataLength);

	if (PropSet == AM_KSPROPSETID_CopyProt) {
		if (Id == AM_PROPERTY_COPY_ANALOG_COMPONENT) {
			if (pPropertyData && DataLength >= sizeof(ULONG) && pBytesReturned) {
				*(ULONG*)pPropertyData = FALSE;
				*pBytesReturned = sizeof(ULONG);
				return S_OK;
			}
			return E_INVALIDARG;
		}
	}
	else {
		return E_PROP_SET_UNSUPPORTED;
	}

	return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP CMpcVideoRenderer::QuerySupported(REFGUID PropSet, ULONG Id, ULONG* pTypeSupport)
{
	DLog(L"IKsPropertySet::QuerySupported({}, ...)", PropSetAndIdToString(PropSet, Id));

	if (PropSet == AM_KSPROPSETID_CopyProt) {
		if (Id == AM_PROPERTY_COPY_MACROVISION || Id == AM_PROPERTY_COPY_DIGITAL_CP) {
			*pTypeSupport = KSPROPERTY_SUPPORT_SET;
			return S_OK;
		}
		if (Id == AM_PROPERTY_COPY_ANALOG_COMPONENT) {
			*pTypeSupport = KSPROPERTY_SUPPORT_GET;
			return S_OK;
		}
	}
	else {
		return E_PROP_SET_UNSUPPORTED;
	}

	return E_PROP_ID_UNSUPPORTED;
}

// IMFGetService
STDMETHODIMP CMpcVideoRenderer::GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject)
{
	if (guidService == MR_VIDEO_ACCELERATION_SERVICE) {
		if (riid == IID_IDirect3DDeviceManager9 && m_VideoProcessor->GetDeviceManager9()) {
			return m_VideoProcessor->GetDeviceManager9()->QueryInterface(riid, ppvObject);
		}
	}
	else if (guidService == MR_VIDEO_MIXER_SERVICE) {
		if (riid == IID_IMFVideoProcessor || riid == IID_IMFVideoMixerBitmap) {
			return m_VideoProcessor->QueryInterface(riid, ppvObject);
		}
	}

	return E_NOINTERFACE;
}

// IBasicVideo
STDMETHODIMP CMpcVideoRenderer::GetSourcePosition(long *pLeft, long *pTop, long *pWidth, long *pHeight)
{
	CheckPointer(pLeft,E_POINTER);
	CheckPointer(pTop,E_POINTER);
	CheckPointer(pWidth,E_POINTER);
	CheckPointer(pHeight,E_POINTER);

	CRect rect;
	long lAspectX;
	long lAspectY;
	{
		CAutoLock cVideoLock(&m_InterfaceLock);

		m_VideoProcessor->GetSourceRect(rect);
		m_VideoProcessor->GetAspectRatio(lAspectX, lAspectY);
	}

	*pLeft = rect.left;
	*pTop = rect.top;
	*pWidth = rect.Width();
	*pHeight = rect.Height();

	if (lAspectX && lAspectY) {
		// Apply aspect ratio (as in VMR9 and madVR) so that DVD-Video menu buttons are activated correctly with the mouse.
		*pWidth = *pHeight * lAspectX / lAspectY;
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::SetDestinationPosition(long Left, long Top, long Width, long Height)
{
	const CRect videoRect(Left, Top, Left + Width, Top + Height);
	if (videoRect.IsRectNull()) {
		return S_OK;
	}

	{
		std::lock_guard lock(m_pendingGeometryLock);
		m_pendingVideoRect = videoRect;
	}
	ApplyPendingGeometry();
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetDestinationPosition(long *pLeft, long *pTop, long *pWidth, long *pHeight)
{
	CheckPointer(pLeft,E_POINTER);
	CheckPointer(pTop,E_POINTER);
	CheckPointer(pWidth,E_POINTER);
	CheckPointer(pHeight,E_POINTER);

	CRect rect;
	{
		CAutoLock cVideoLock(&m_InterfaceLock);

		m_VideoProcessor->GetVideoRect(rect);
	}

	*pLeft = rect.left;
	*pTop = rect.top;
	*pWidth = rect.Width();
	*pHeight = rect.Height();

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetVideoSize(long *pWidth, long *pHeight)
{
	CheckPointer(pWidth, E_POINTER);
	CheckPointer(pHeight, E_POINTER);

	// retrieves the native video's width and height.
	m_VideoProcessor->GetVideoSize(*pWidth, *pHeight);

	long lAspectX;
	long lAspectY;

	m_VideoProcessor->GetAspectRatio(lAspectX, lAspectY);
	if (lAspectX && lAspectY) {
		// Apply aspect ratio (as in VMR9 and madVR) so that DVD-Video menu buttons are activated correctly with the mouse.
		*pWidth = static_cast<long>(llMulDiv(*pHeight, lAspectX, lAspectY, 0));
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetCurrentImage(long *pBufferSize, long *pDIBImage)
{
	CheckPointer(pBufferSize, E_POINTER);

	CAutoLock cVideoLock(&m_InterfaceLock);
	CAutoLock cRendererLock(&m_RendererLock);
	HRESULT hr;

	CSize framesize;
	long aspectX, aspectY;
	int iRotation;

	m_VideoProcessor->GetVideoSize(framesize.cx, framesize.cy);
	m_VideoProcessor->GetAspectRatio(aspectX, aspectY);
	iRotation = m_VideoProcessor->GetRotation();

	if (aspectX > 0 && aspectY > 0) {
		if (iRotation == 90 || iRotation == 270) {
			framesize.cy = MulDiv(framesize.cx, aspectY, aspectX);
		} else {
			framesize.cx = MulDiv(framesize.cy, aspectX, aspectY);
		}
	}

	if (const auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
		dx11 && dx11->CanProjectJvr()) {
		framesize = CSize(jvr::FrameWidth(dx11->m_srcRectHeight), dx11->m_srcRectHeight);
	}

	const auto w = framesize.cx;
	const auto h = framesize.cy;

	// VFW_E_NOT_PAUSED ?

	if (w <= 0 || h <= 0) {
		return E_FAIL;
	}
	long size = sizeof(BITMAPINFOHEADER) + CalcDibRowPitch(w, 32) * h;

	if (pDIBImage == nullptr) {
		*pBufferSize = size;
		return S_OK;
	}

	if (size > *pBufferSize) {
		return E_OUTOFMEMORY;
	}

	hr = m_VideoProcessor->GetCurentImage(pDIBImage);

	return hr;
}

// IBasicVideo2
STDMETHODIMP CMpcVideoRenderer::GetPreferredAspectRatio(long *plAspectX, long *plAspectY)
{
	CheckPointer(plAspectX, E_POINTER);
	CheckPointer(plAspectY, E_POINTER);

	m_VideoProcessor->GetAspectRatio(*plAspectX, *plAspectY);

	return S_OK;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	CMpcVideoRenderer* pThis = reinterpret_cast <CMpcVideoRenderer*>(GetWindowLongPtrW(hwnd, 0));
	if (!pThis) {
		if ((uMsg != WM_NCCREATE)
				|| (nullptr == (pThis = (CMpcVideoRenderer*)((LPCREATESTRUCTW)lParam)->lpCreateParams))) {
			return DefWindowProcW(hwnd, uMsg, wParam, lParam);
		}

		SetWindowLongPtrW(hwnd, 0, reinterpret_cast<LONG_PTR>(pThis));
	}

	return pThis->OnReceiveMessage(hwnd, uMsg, wParam, lParam);
}

HRESULT CMpcVideoRenderer::Init(const bool bCreateWindow/* = false*/)
{
	CAutoLock cRendererLock(&m_RendererLock);

	HRESULT hr = S_OK;

	auto hwnd = m_hWndParent;
	while ((GetParent(hwnd)) && (GetParent(hwnd) == GetAncestor(hwnd, GA_PARENT))) {
		hwnd = GetParent(hwnd);
	}

	if (hwnd != m_hWndParentMain) {
		if (m_hWndParentMain) {
			RemoveParentWndProc(m_hWndParentMain);
		}

		m_hWndParentMain = hwnd;

		auto lpPreviousProc = SetWindowLongPtrW(m_hWndParentMain, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ParentWndProc));
		if (lpPreviousProc && lpPreviousProc != reinterpret_cast<LONG_PTR>(ParentWndProc)) {
			SetPropW(m_hWndParentMain, g_pszOldParentWndProc, reinterpret_cast<HANDLE>(lpPreviousProc));
			SetPropW(m_hWndParentMain, g_pszThis, reinterpret_cast<HANDLE>(this));
		} else {
			ASSERT(false);
		}
	}

	if (bCreateWindow) {
		const HWND hWndOld = m_hWndWindow;
		m_hWndWindow = nullptr;

		if (!m_bIsD3DFullscreen) {
			WNDCLASSEXW wc = { sizeof(wc) };
			if (!GetClassInfoExW(g_hInst, RendererWindowClassName, &wc)) {
				wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
				wc.lpfnWndProc = WndProc;
				wc.hInstance = g_hInst;
				wc.lpszClassName = RendererWindowClassName;
				wc.cbWndExtra = sizeof(CMpcVideoRenderer*); // pointer size
				if (!RegisterClassExW(&wc)) {
					hr = HRESULT_FROM_WIN32(GetLastError());
					DLog(L"CMpcVideoRenderer::Init() : RegisterClassExW() failed with error {}", HR2Str(hr));
					return hr;
				}
			}

			// The window is made again on the thread that made it the first time. A
			// player that opens the film on a worker thread has it there, and a
			// setting applied from the options arrives on the interface thread: made
			// here, the new window would change threads, and MPC-HC, which tracks the
			// mouse leaving its video area, is then told the pointer has left while
			// it sits on the picture -- and never hides it again. If that thread does
			// not answer, the window is made here as it always was.
			const bool bDisabled = hWndOld && !IsWindowEnabled(hWndOld);
			if (hWndOld && GetWindowThreadProcessId(hWndOld, nullptr) != GetCurrentThreadId()) {
				DWORD_PTR made = 0;
				if (SendMessageTimeoutW(hWndOld, WM_VR_REMAKE, bDisabled, 0, SMTO_ABORTIFHUNG, 2000, &made)) {
					m_hWndWindow = reinterpret_cast<HWND>(made);
				}
				DLogIf(!m_hWndWindow, L"CMpcVideoRenderer::Init() : the window's thread did not make the new one");
			}
			if (hWndOld) {
				::SendMessageW(hWndOld, WM_CLOSE, 0, 0);
			}
			if (!m_hWndWindow) {
				m_hWndWindow = CreateVideoWindow(bDisabled);
			}

			if (!m_hWndWindow) {
				hr = HRESULT_FROM_WIN32(GetLastError());
				DLog(L"CMpcVideoRenderer::Init() : CreateWindowExW() failed with error {}", HR2Str(hr));
				return E_FAIL;
			}

			if (!m_windowRect.IsRectNull()) {
				SetWindowPos(m_hWndWindow, nullptr, m_windowRect.left, m_windowRect.top, m_windowRect.Width(), m_windowRect.Height(), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
			}
		} else if (hWndOld) {
			::SendMessageW(hWndOld, WM_CLOSE, 0, 0);
		}
	}

	m_hWnd = m_bExclusiveScreen && m_VideoProcessor->Type() == VP_DX9 ? m_hWndParentMain : m_hWndWindow;
	if (m_bIsD3DFullscreen) {
		m_hWnd = m_hWndParent;
	}

	InstallToggleKeyHook();

	bool bChangeDevice = false;
	hr = m_VideoProcessor->Init(m_hWnd, false, &bChangeDevice);
	if (bChangeDevice) {
		DoAfterChangingDevice();
	}

	return hr;
}

// IVideoWindow
STDMETHODIMP CMpcVideoRenderer::put_Owner(OAHWND Owner)
{
	if (Owner && m_hWndParent != (HWND)Owner) {
		m_hWndParent = (HWND)Owner;
		return Init(true);
	}

	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::get_Owner(OAHWND *Owner)
{
	CheckPointer(Owner, E_POINTER);
	*Owner = (OAHWND)m_hWndParent;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::put_MessageDrain(OAHWND Drain)
{
	if (m_pInputPin == nullptr || m_pInputPin->IsConnected() == FALSE) {
		return VFW_E_NOT_CONNECTED;
	}
	m_hWndDrain = (HWND)Drain;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::get_MessageDrain(OAHWND* Drain)
{
	CheckPointer(Drain, E_POINTER);
	if (m_pInputPin == nullptr || m_pInputPin->IsConnected() == FALSE) {
		return VFW_E_NOT_CONNECTED;
	}
	*Drain = (OAHWND)m_hWndDrain;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::SetWindowPosition(long Left, long Top, long Width, long Height)
{
	const CRect windowRect(Left, Top, Left + Width, Top + Height);
	{
		std::lock_guard lock(m_pendingGeometryLock);
		m_pendingWindowRect = windowRect;
	}
	ApplyPendingGeometry();
	return S_OK;
}

HRESULT CMpcVideoRenderer::ApplyWindowPosition(const CRect& windowRect)
{
	if (windowRect == m_windowRect) {
		return S_OK;
	}

	m_windowRect = windowRect;
	const auto Left = windowRect.left;
	const auto Top = windowRect.top;
	const auto Width = windowRect.Width();
	const auto Height = windowRect.Height();

	std::optional<bool> bFullScreenOpt;

	if (!m_bIsD3DFullscreen) {
		auto hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { mi.cbSize = sizeof(mi) };
		::GetMonitorInfoW(hMon, &mi);
		const CRect rcMonitor(mi.rcMonitor);
		auto fullScreen = (m_windowRect.Width() == rcMonitor.Width() && m_windowRect.Height() == rcMonitor.Height());
		if (m_Sets.bExclusiveFS || m_bExclusiveScreen) {
			auto SwitchExclusiveScreen = [this] (bool set) {
				DLog(L"CMpcVideoRenderer::SetWindowPosition() : Switch {} Exclusive screen", set ? L"to" : L"from");
				m_bExclusiveScreen = set;

				if (m_hWnd) {
					Init(m_VideoProcessor->Type() == VP_DX9 ? false : true);
					Redraw();

					if (m_hWndParentMain) {
						PostMessageW(m_hWndParentMain, WM_SWITCH_FULLSCREEN, static_cast<WPARAM>(set), 0);
					}
				}
			};

			if (!m_bExclusiveScreen && fullScreen) {
				SwitchExclusiveScreen(true);
			} else if (m_bExclusiveScreen && !fullScreen) {
				SwitchExclusiveScreen(false);
			}
		} else {
			if (!m_bFullScreen && fullScreen) {
				bFullScreenOpt = true;
			} else if (m_bFullScreen && !fullScreen) {
				bFullScreenOpt = false;
			}
		}
	}

	if (m_hWndWindow && !m_bExclusiveScreen) {
		SetWindowPos(m_hWndWindow, nullptr, Left, Top, Width, Height,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_ASYNCWINDOWPOS);
		if (Left < 0) {
			m_windowRect.OffsetRect(-Left, 0);
		}
		if (Top < 0) {
			m_windowRect.OffsetRect(0, -Top);
		}
	}

	m_VideoProcessor->SetWindowRect(m_windowRect);

	m_windowRect = windowRect;

	if (bFullScreenOpt.has_value()) {
		m_bFullScreen = *bFullScreenOpt;
		DLog(L"CMpcVideoRenderer::SetWindowPosition() : Switch {} FullScreen", m_bFullScreen ? L"to" : L"from");

		m_VideoProcessor->SwitchFullScreen(m_bFullScreen);
	}

	return S_OK;
}

// ISpecifyPropertyPages
STDMETHODIMP CMpcVideoRenderer::GetPages(CAUUID* pPages)
{
	CheckPointer(pPages, E_POINTER);

	static const GUID guidQualityPPage = { 0x565DCEF2, 0xAFC5, 0x11D2, 0x88, 0x53, 0x00, 0x00, 0xF8, 0x08, 0x83, 0xE3 };

	// One owner for original MPC-VR settings and MFG: the original Main page.
	// Keep the MFG-only page compiled for compatibility with old COM registration,
	// but do not expose two independently applying copies of Settings_t.
	const bool bActive = GetActive();
	pPages->cElems = 1 + (bActive ? 2 : 0);
	pPages->pElems = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID) * pPages->cElems));
	if (pPages->pElems == nullptr) {
		return E_OUTOFMEMORY;
	}

	ULONG n = 0;
	pPages->pElems[n++] = __uuidof(CVRMainPPage);
	if (bActive) {
		pPages->pElems[n++] = __uuidof(CVRInfoPPage);
		pPages->pElems[n++] = guidQualityPPage;
	}

	return S_OK;
}

// IVideoRenderer

STDMETHODIMP CMpcVideoRenderer::GetVideoProcessorInfo(std::wstring& str)
{
	CAutoLock guard(&m_RendererLock);
	const auto hr=m_VideoProcessor->GetVPInfo(str);
	str += std::format(L"\nRenderer requested MFG: {}", m_Sets.iMfgMultiplier >= 2 ? L"Auto" : L"Off");
	str += L"\n" + m_VideoProcessor->GetMfgStatus();
	if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) str += dx11->CompletedBufferInfo();
	return hr;
}

STDMETHODIMP_(bool) CMpcVideoRenderer::GetActive()
{
	return m_pInputPin && m_pInputPin->GetConnected();
}

STDMETHODIMP_(void) CMpcVideoRenderer::GetSettings(Settings_t& setings)
{
	setings = m_Sets;
}

STDMETHODIMP_(void) CMpcVideoRenderer::SetSettings(const Settings_t& setings)
{
	CAutoLock cRendererLock(&m_RendererLock);

	m_Sets = setings;
	NormalizeEditionSettings();
	m_VideoProcessor->Configure(m_Sets);
	OnEditionSettingsConfigured();
}

STDMETHODIMP CMpcVideoRenderer::SaveSettings()
{
	// Snapshot under the renderer lock; the registry write must not hold the GPU
	// presentation lock. Persistence does not depend on NGX initialization.
	Settings_t snapshot;
    DWORD videoEnhancement = 0;
    {
        CAutoLock lock(&m_RendererLock);
        snapshot = m_Sets;
        videoEnhancement = m_videoEnhancement.load() ? 1u : 0u;
    }
	CRegKey key;
	const LSTATUS opened = key.Create(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER);
	if (opened != ERROR_SUCCESS) return HRESULT_FROM_WIN32(opened);
	WriteSettings(key, snapshot);
    // Preview is in-memory; only Apply/Save persists this one setting.
    const auto stored = key.SetDWORDValue(L"VideoEnhancement", videoEnhancement);
    if (stored != ERROR_SUCCESS) return HRESULT_FROM_WIN32(stored);
    DWORD verify = DWORD(-1);
    if (key.QueryDWORDValue(L"VideoEnhancement", verify) != ERROR_SUCCESS || verify != videoEnhancement)
        return E_FAIL;
	return edition::VerifySavedSettings(key, snapshot);
}

// ISubRender (DX9)
STDMETHODIMP CMpcVideoRenderer::SetCallback(ISubRenderCallback* cb)
{
	m_pSubCallBack = cb;

	return S_OK;
}

// ISubRender11 (DX11)
STDMETHODIMP CMpcVideoRenderer::SetCallback11(ISubRender11Callback* cb)
{
	m_pSub11CallBack = cb;

	return S_OK;
}

// IExFilterConfig

STDMETHODIMP CMpcVideoRenderer::Flt_GetBool(LPCSTR field, bool* value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "statsEnable")) {
		*value = m_Sets.bShowStats;
		return S_OK;
	}

	if (const auto result = GetEditionBool(field, value)) return *result;

	if (!strcmp(field, "flip")) {
		*value = m_VideoProcessor->GetFlip();
		return S_OK;
	}

	if (!strcmp(field, "doubleRate")) {
		CAutoLock cRendererLock(&m_RendererLock);
		*value = m_VideoProcessor->GetDoubleRate();
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetInt(LPCSTR field, int* value)
{
	CheckPointer(value, E_POINTER);

	if (const auto result = GetEditionInt(field, value)) return *result;

	if (!strcmp(field, "renderType")) {
		if (m_inputMT.IsValid()) {
			*value = m_VideoProcessor->Type();
		} else {
			*value = 0; // not initialized
		}
		return S_OK;
	}
	if (!strcmp(field, "playbackState")) {
		*value = m_filterState;
		return S_OK;
	}
	if (!strcmp(field, "rotation")) {
		*value = m_VideoProcessor->GetRotation();
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetInt64(LPCSTR field, __int64 *value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "version")) {
		*value  = ((uint64_t)VER_MAJOR << 48)
				| ((uint64_t)VER_MINOR << 32)
				| ((uint64_t)VER_BUILD << 16)
				| ((uint64_t)REV_NUM);
		return S_OK;
	} else if (!strcmp(field, "originalVideoSize")) {
		SIZE size;
		m_VideoProcessor->GetVideoSize(size.cx, size.cy);

		static_assert(sizeof(*value) == sizeof(size));
		*value = *(__int64*)&size;

		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_GetBin(LPCSTR field, LPVOID* value, unsigned* size)
{
	if (!strcmp(field, "displayedImage")) {
		if (m_State != State_Running) {
			Redraw();
		}

		CAutoLock cRendererLock(&m_RendererLock);

		HRESULT hr = m_VideoProcessor->GetDisplayedImage((BYTE**)value, size);

		return hr;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_SetBool(LPCSTR field, bool value)
{
	if (!strcmp(field, "cmd_redraw") && value) {
		Redraw();
		return S_OK;
	}

	if (const auto result = SetEditionBool(field, value)) return *result;

	if (!strcmp(field, "cmd_clearPreScaleShaders") && value) {
		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->ClearPreScaleShaders();
		return S_OK;
	}

	if (!strcmp(field, "cmd_clearPostScaleShaders") && value) {
		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->ClearPostScaleShaders();
		return S_OK;
	}

	if (!strcmp(field, "statsEnable")) {
		m_Sets.bShowStats = value;
		m_VideoProcessor->SetShowStats(m_Sets.bShowStats);

		SaveSettings();
		if (m_filterState != State_Running) {
			Redraw();
		}
		return S_OK;
	}

	if (!strcmp(field, "lessRedraws")) {
		m_bForceRedrawing = !value;
		return S_OK;
	}

	if (!strcmp(field, "d3dFullscreenControl")) {
		m_bEnableFullscreenControl = value;
		return S_OK;
	}

	if (!strcmp(field, "flip")) {
		CAutoLock cRendererLock(&m_RendererLock);

		m_VideoProcessor->SetFlip(value);
		return S_OK;
	}

	if (!strcmp(field, "allowDeepColorBitmaps")) {
		m_VideoProcessor->SetAllowDeepColorBitmaps(value);
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_SetInt(LPCSTR field, int value)
{
	if (const auto result = SetEditionInt(field, value)) return *result;

	if (!strcmp(field, "rotation")) {
		// Allowed angles are multiples of 90.
		if (value % 90 == 0) {
			// The allowed rotation angle is reduced to 0, 90, 180, 270.
			value %= 360;
			if (value < 0) {
				value += 360;
			}

			CAutoLock cRendererLock(&m_RendererLock);

			m_VideoProcessor->SetRotation(value);
			return S_OK;
		}
	}

	if (!strcmp(field, "stereo3dTransform")) {
		if (value == STEREO3D_AsIs || value == STEREO3D_HalfOverUnder_to_Interlace) {
			CAutoLock cRendererLock(&m_RendererLock);

			m_VideoProcessor->SetStereo3dTransform(value);
			return S_OK;
		}
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::Flt_SetBin(LPCSTR field, LPVOID value, int size)
{
	if (size > 0) {
		auto ReadShaderData = [&](std::wstring& shaderName, std::string& shaderCode) {
			BYTE* p = (BYTE*)value;
			const BYTE* end = p + size;
			uint32_t chunkcode;
			int32_t chunksize;

			while (p + 8 < end) {
				memcpy(&chunkcode, p, 4);
				p += 4;
				memcpy(&chunksize, p, 4);
				p += 4;
				if (chunksize <= 0 || p + chunksize > end) {
					break;
				}

				switch (chunkcode) {
				case FCC('NAME'):
					shaderName.assign((LPCWSTR)p, chunksize / sizeof(wchar_t));
					break;
				case FCC('CODE'):
					shaderCode.assign((LPCSTR)p, chunksize);
					break;
				}
				p += chunksize;
			}
		};

		if (!strcmp(field, "cmd_addPreScaleShader")) {
			std::wstring shaderName;
			std::string shaderCode;

			ReadShaderData(shaderName, shaderCode);

			if (shaderCode.size()) {
				CAutoLock cRendererLock(&m_RendererLock);

				return m_VideoProcessor->AddPreScaleShader(shaderName, shaderCode);
			}
		}

		if (!strcmp(field, "cmd_addPostScaleShader")) {
			std::wstring shaderName;
			std::string shaderCode;

			ReadShaderData(shaderName, shaderCode);

			if (shaderCode.size()) {
				CAutoLock cRendererLock(&m_RendererLock);

				return m_VideoProcessor->AddPostScaleShader(shaderName, shaderCode);
			}
		}
	}

	return E_INVALIDARG;
}

// ID3DFullscreenControl

STDMETHODIMP CMpcVideoRenderer::SetD3DFullscreen(bool bEnabled)
{
	m_bExclusiveScreen = m_bIsD3DFullscreen = bEnabled;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetD3DFullscreen(bool* pbEnabled)
{
	CheckPointer(pbEnabled, E_POINTER);
	*pbEnabled = m_bIsD3DFullscreen;
	return S_OK;
}

// ISubRenderConsumer2

STDMETHODIMP CMpcVideoRenderer::Clear(REFERENCE_TIME clearNewerThan /* = 0 */)
{
	DLog(L"ISubRenderConsumer2::Clear");
	return m_pSubPicQueue->Invalidate(clearNewerThan);
}

// ISubRenderConsumer

STDMETHODIMP CMpcVideoRenderer::GetMerit(ULONG* plMerit)
{
	DLog(L"ISubRenderConsumer::GetMerit");
	CheckPointer(plMerit, E_POINTER);
	*plMerit = 4 << 16;
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::Connect(ISubRenderProvider* subtitleRenderer)
{
	DLog(L"ISubRenderConsumer::Connect");

	if (m_pSubPicProvider) {
		return E_ABORT;
	}

	HRESULT hr = subtitleRenderer->SetBool("combineBitmaps", true);
	if (FAILED(hr)) {
		return hr;
	}

	if (CComQIPtr<ISubRenderConsumer> pSubConsumer = m_pSubPicQueue.p) {
		hr = pSubConsumer->Connect(subtitleRenderer);
	}
	else {
		ISubPicAllocator* pSubPicAllocator = m_VideoProcessor->GetSubPicAllocator();
		if (!pSubPicAllocator) {
			return E_FAIL;
		}

		CComPtr<ISubPicProvider> pSubPicProvider = (ISubPicProvider*)new CXySubPicProvider(subtitleRenderer);
		CComPtr<ISubPicQueue> pSubPicQueue       = (ISubPicQueue*)new CXySubPicQueueNoThread(pSubPicAllocator, &hr);

		if (SUCCEEDED(hr)) {
			CAutoLock cAutoLock(&m_InterfaceLock);
			pSubPicQueue->SetSubPicProvider(pSubPicProvider);
			m_pSubPicProvider = pSubPicProvider;
			m_pSubPicQueue = pSubPicQueue;

			pSubPicAllocator->SetInverseAlpha(true);
		}
	}

	return hr;
}

STDMETHODIMP CMpcVideoRenderer::Disconnect()
{
	DLog(L"ISubRenderConsumer::Disconnect");
	m_pSubPicProvider.Release();
	return m_pSubPicQueue->SetSubPicProvider(m_pSubPicProvider);
}

STDMETHODIMP CMpcVideoRenderer::DeliverFrame(REFERENCE_TIME start, REFERENCE_TIME stop, LPVOID context, ISubRenderFrame* subtitleFrame)
{
	//DLog(L"ISubRenderConsumer::DeliverFrame");
	HRESULT hr = E_FAIL;

	if (CComQIPtr<IXyCompatProvider> pXyProvider = m_pSubPicProvider.p) {
		hr = pXyProvider->DeliverFrame(start, stop, context, subtitleFrame);
	}

	return hr;
}

// ISubRenderOptions

STDMETHODIMP CMpcVideoRenderer::GetSize(LPCSTR field, SIZE* value)
{
	CheckPointer(value, E_POINTER);

	if (!strcmp(field, "originalVideoSize")) {
		*value = m_videoSize;
		return S_OK;
	}
	else if (!strcmp(field, "arAdjustedVideoSize")) {
		*value = m_videoSize;
		if (m_videoAspectRatio.cx > 0 && m_videoAspectRatio.cy > 0) {
			value->cx = MulDiv(m_videoSize.cx, m_videoAspectRatio.cx, m_videoAspectRatio.cy);
		}
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetRect(LPCSTR field, RECT* value)
{
	CheckPointer(value, E_POINTER);
	if (!strcmp(field, "videoOutputRect") || !strcmp(field, "subtitleTargetRect")) {
		if (m_videoRect.IsRectEmpty()) {
			if (m_windowRect.IsRectEmpty()) {
				*value = { 0, 0, 1280, 720 };
			}
			else {
				*value = m_windowRect;
			}
		}
		else {
			value->left = 0;
			value->top = 0;
			value->right = m_videoRect.Width();
			value->bottom = m_videoRect.Height();
		}
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetUlonglong(LPCSTR field, ULONGLONG* value)
{
	CheckPointer(value, E_POINTER);
	if (!strcmp(field, "frameRate")) {
		// TODO: check it
		*value = (REFERENCE_TIME)(10000000.0 / m_FrameStats.GetAverageFps());
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetDouble(LPCSTR field, double* value)
{
	CheckPointer(value, E_POINTER);
	if (!strcmp(field, "refreshRate")) {
		// TODO: check it
		*value = 1000.0 / m_FrameStats.GetAverageFps();
		// hmm, calculate Refresh Time in milliseconds (not Refresh Rate)
		return S_OK;
	}

	return E_INVALIDARG;
}

STDMETHODIMP CMpcVideoRenderer::GetString(LPCSTR field, LPWSTR* value, int* chars)
{
	CheckPointer(value, E_POINTER);
	CheckPointer(chars, E_POINTER);
	std::wstring str;

	if (!strcmp(field, "name")) {
		str = L"MPC-VR-Edition";
	}
	else if (!strcmp(field, "version")) {
		str = _CRT_WIDE(VERSION_STR);
	}
	else if (!strcmp(field, "yuvMatrix")) {
		auto exFmt = m_VideoProcessor->getSrcExFormat();

		switch (exFmt.VideoTransferMatrix) {
		default:
		case DXVA2_VideoTransferMatrix_BT709: str = L"TV.709"; break;
		case VIDEOTRANSFERMATRIX_FCC:
		case DXVA2_VideoTransferMatrix_BT601: str = L"TV.601"; break;
		case MFVideoTransferMatrix_BT2020_10: str = L"TV.2020"; break;
		}
	}

	if (str.length()) {
		const int len = str.length();
		const size_t sz = (len + 1) * sizeof(WCHAR);
		LPWSTR buf = (LPWSTR)LocalAlloc(LPTR, sz);

		if (!buf) {
			return E_OUTOFMEMORY;
		}

		wcscpy_s(buf, len + 1, str.data());
		*chars = len;
		*value = buf;

		return S_OK;
	}

	return E_INVALIDARG;
}

CComPtr<ISubPic> CMpcVideoRenderer::GetSubPic(REFERENCE_TIME rtStart)
{
	CComPtr<ISubPic> pSubPic;
	if (m_pSubPicQueue) {
		const auto rtNow = m_rtStartTime + rtStart;
		bool ret = m_pSubPicQueue->LookupSubPic(rtNow, m_filterState == State_Running, pSubPic);
	}
	return pSubPic;
}

HRESULT CMpcVideoRenderer::Redraw()
{
	CAutoLock cRendererLock(&m_RendererLock);
	const auto bDrawFrame = m_bValidBuffer && m_filterState != State_Stopped;

	HRESULT hr = S_OK;
	if (bDrawFrame) {
		hr = m_VideoProcessor->Render(0, INVALID_TIME);
	} else {
		hr = m_VideoProcessor->FillBlack();
	}

	return hr;
}

void CMpcVideoRenderer::DoAfterChangingDevice()
{
	if (m_pInputPin->IsConnected() == TRUE && m_pSink) {
		DLog(L"CMpcVideoRenderer::DoAfterChangingDevice()");
		m_bValidBuffer = false;
		auto pPin = (IPin*)m_pInputPin;
		m_pInputPin->AddRef();
		EXECUTE_ASSERT(S_OK == m_pSink->Notify(EC_DISPLAY_CHANGED, (LONG_PTR)pPin, 0));
		SetAbortSignal(TRUE);
		SAFE_RELEASE(m_pMediaSample);
		m_pInputPin->Release();
	}
}

LRESULT CMpcVideoRenderer::OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (const auto result = HandleEditionWindowMessage(hwnd, uMsg, wParam, lParam)) return *result;

	if (m_hWndDrain && !InSendMessage() && !m_bExclusiveScreen) {
		switch (uMsg) {
			case WM_CHAR:
			case WM_DEADCHAR:
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_LBUTTONDBLCLK:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_MBUTTONDBLCLK:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MOUSEACTIVATE:
			case WM_MOUSEMOVE:
			case WM_NCLBUTTONDBLCLK:
			case WM_NCLBUTTONDOWN:
			case WM_NCLBUTTONUP:
			case WM_NCMBUTTONDBLCLK:
			case WM_NCMBUTTONDOWN:
			case WM_NCMBUTTONUP:
			case WM_NCMOUSEMOVE:
			case WM_NCRBUTTONDBLCLK:
			case WM_NCRBUTTONDOWN:
			case WM_NCRBUTTONUP:
			case WM_RBUTTONDBLCLK:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_XBUTTONDBLCLK:
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
			case WM_SYSCHAR:
			case WM_SYSDEADCHAR:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
				PostMessageW(m_hWndDrain, uMsg, wParam, lParam);
				return 0L;
		}
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
