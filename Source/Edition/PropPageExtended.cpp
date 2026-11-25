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
#include "resource.h"
#include "Helper.h"
#include "PropPage.h"
#include "JvrProjection.h"
#include "Edition/SettingsRegistry.h"
#include "Times.h"
#include "Mfg/CadenceTrace.h"
#include <FilterInterfaces.h>

void CVRMainPPage::UpdateJvrViewLabels()
{
	const int vertical = m_SetsPP.iJvrVertical;
	SetDlgItemTextW(IDC_JVR_VERTICAL_VALUE, std::format(L"Vertical framing: {}{}%", vertical > 0 ? L"+" : L"", vertical).c_str());
	SetDlgItemTextW(IDC_JVR_REPROJECTION_VALUE, std::format(L"Reprojection: {}%", m_SetsPP.iJvrReprojection).c_str());
}

void CVRMainPPage::ApplyJvrPreview()
{
	Settings_t live;
	m_pVideoRenderer->GetSettings(live);
	if (live.bJvrFlatView == m_SetsPP.bJvrFlatView && live.iJvrVertical == m_SetsPP.iJvrVertical &&
		live.iJvrReprojection == m_SetsPP.iJvrReprojection && live.iJvrEye == m_SetsPP.iJvrEye) return;
	live.bJvrFlatView = m_SetsPP.bJvrFlatView;
	live.iJvrVertical = m_SetsPP.iJvrVertical;
	live.iJvrReprojection = m_SetsPP.iJvrReprojection;
	live.iJvrEye = m_SetsPP.iJvrEye;
	m_pVideoRenderer->SetSettings(live);
}

// [VE-00 | 2026-11-24 16:39:20 +08:00] Preview single feature, commit on Apply, rollback on Cancel.
bool CVRMainPPage::PreviewVideoEnhancement(bool enable)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (!config || FAILED(config->Flt_SetBool("videoEnhancement", enable))) return false;
    m_videoEnhancement = enable;
    return true;
}

void CVRMainPPage::SetEditionControls()
{
	CheckDlgButton(IDC_JVR_FLAT_VIEW, m_SetsPP.bJvrFlatView ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETPOS, TRUE, m_SetsPP.iJvrVertical);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETPOS, TRUE, m_SetsPP.iJvrReprojection);
	ComboBox_SelectByItemData(m_hWnd, IDC_JVR_EYE, m_SetsPP.iJvrEye);
	UpdateJvrViewLabels();

	CheckDlgButton(IDC_MFG_MULTIPLIER, m_SetsPP.iMfgMultiplier >= 2 ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SelectByItemData(m_hWnd, IDC_ZIPDEPTH_MODE, edition::ZipDepthModeFromSettings(m_SetsPP));
	CheckDlgButton(IDC_SHOW_MFG_MOTION, m_SetsPP.bShowMfgMotion ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_VIDEO_ENHANCEMENT, m_videoEnhancement ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_SHOW_DEPTH_OVERLAY, m_SetsPP.bShowDepthOverlay ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_MFG_FEATURE_BADGES, m_SetsPP.bShowFeatureBadges ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_EDITION_STATS_DETAILS, m_SetsPP.bShowEditionDetails ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SelectByItemData(m_hWnd, IDC_MFG_PROCESSING_MODE, m_SetsPP.iMfgProcessingMode);
	ComboBox_SelectByItemData(m_hWnd, IDC_EXTENDED_STARTUP, m_SetsPP.bWaitForExtendedStartup);

    ComboBox_SelectByItemData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT, m_SetsPP.iDlssSRSourceLimit);
}

void CVRMainPPage::EnableEditionControls()
{
	for (const int id : {IDC_JVR_FLAT_VIEW, IDC_JVR_PROFILE_NAME, IDC_JVR_EYE, IDC_JVR_VERTICAL,
		IDC_JVR_VERTICAL_VALUE, IDC_JVR_REPROJECTION, IDC_JVR_REPROJECTION_VALUE}) {
		GetDlgItem(id).ShowWindow(m_showJvrOptions ? SW_SHOW : SW_HIDE);
	}
	if (m_showJvrOptions) {
		CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
		LPWSTR name = nullptr;
		if (config) config->Flt_GetString("projectionProfileName", &name, nullptr);
		SetDlgItemTextW(IDC_JVR_PROFILE_NAME, name ? name : L"");
		CoTaskMemFree(name);
	}
	GetDlgItem(IDC_JVR_FLAT_VIEW).EnableWindow(m_SetsPP.bUseD3D11);
	const bool jvrEnabled = m_SetsPP.bUseD3D11 && m_SetsPP.bJvrFlatView;
	for (const int id : {IDC_JVR_VERTICAL, IDC_JVR_VERTICAL_VALUE, IDC_JVR_REPROJECTION,
		IDC_JVR_REPROJECTION_VALUE, IDC_JVR_EYE}) {
		GetDlgItem(id).EnableWindow(jvrEnabled);
	}
	const BOOL bMfgEnabled = m_SetsPP.bUseD3D11 && m_SetsPP.iMfgMultiplier >= 2 && IsWindows10OrGreater();
	GetDlgItem(IDC_MFG_MULTIPLIER).EnableWindow(m_SetsPP.bUseD3D11 && IsWindows10OrGreater());
	GetDlgItem(IDC_MFG_PROCESSING_MODE).EnableWindow(bMfgEnabled);
	const BOOL canUseDlssSR = m_SetsPP.bUseD3D11 && IsWindows10OrGreater();
	GetDlgItem(IDC_EDITION_STATS_DETAILS).EnableWindow(m_SetsPP.bUseD3D11);
	const bool srEnabled = canUseDlssSR && m_SetsPP.bDlssSR;
	GetDlgItem(IDC_ZIPDEPTH_MODE).EnableWindow(canUseDlssSR);
    GetDlgItem(IDC_VIDEO_ENHANCEMENT).EnableWindow(canUseDlssSR);
	GetDlgItem(IDC_SHOW_MFG_MOTION).EnableWindow(bMfgEnabled);
	const bool depthAvailable = (srEnabled && m_SetsPP.bUseZipDepthSr) ||
		(bMfgEnabled && m_SetsPP.bUseZipDepthMfg);
	GetDlgItem(IDC_SHOW_DEPTH_OVERLAY).EnableWindow(depthAvailable);
	GetDlgItem(IDC_DLSS_SR_SOURCE_LIMIT).EnableWindow(canUseDlssSR);
}

void CVRMainPPage::RestorePreviewSettings()
{
	auto& trace = mfg::SourceCadenceTrace();
	const auto started = trace.Enabled() ? GetPreciseTick() : 0;
	Settings_t live;
	m_pVideoRenderer->GetSettings(live);
	const bool restoreSettings = live.iSDRDisplayNits != m_oldSDRDisplayNits || live.bJvrFlatView != m_oldJvrFlatView ||
		live.iJvrVertical != m_oldJvrVertical || live.iJvrReprojection != m_oldJvrReprojection ||
		live.iJvrEye != m_oldJvrEye;
    const bool restoreSvdLut = m_videoEnhancementCaptured && m_videoEnhancement != m_oldVideoEnhancement;
	if (restoreSettings) {
		// Roll back the fields that can be previewed before Apply.
		live.iSDRDisplayNits = m_oldSDRDisplayNits;
		live.bJvrFlatView = m_oldJvrFlatView;
		live.iJvrVertical = m_oldJvrVertical;
		live.iJvrReprojection = m_oldJvrReprojection;
		live.iJvrEye = m_oldJvrEye;
		m_pVideoRenderer->SetSettings(live);
	}

    if (restoreSvdLut) PreviewVideoEnhancement(m_oldVideoEnhancement);
    m_videoEnhancementCaptured = false;
	if (trace.Enabled()) {
		const auto finished = GetPreciseTick();
		trace.Record(restoreSettings || restoreSvdLut ? "property_page_close_restore" : "property_page_close_unchanged",
			finished, -1, -1, 0, 0, 0, 0, 0, false, 0, S_OK, finished - started);
	}
}

void CVRMainPPage::CapturePreviewSettings()
{
	m_showJvrOptions = !jvr::ProfileFiles(g_hInst).empty();
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (!m_videoEnhancementCaptured && config &&
        SUCCEEDED(config->Flt_GetBool("videoEnhancement", &m_videoEnhancement))) {
        m_oldVideoEnhancement = m_videoEnhancement;
        m_videoEnhancementCaptured = true;
    }
	m_oldJvrFlatView = m_SetsPP.bJvrFlatView;
	m_oldJvrVertical = m_SetsPP.iJvrVertical;
	m_oldJvrReprojection = m_SetsPP.iJvrReprojection;
	m_oldJvrEye = m_SetsPP.iJvrEye;

}

HRESULT CVRMainPPage::InitializeEditionControls()
{
    ComboBox_AddStringData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT, L"Disable", DLSSSR_SOURCE_Disable);
    ComboBox_AddStringData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS for SD", DLSSSR_SOURCE_SD);
    ComboBox_AddStringData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS \x2264 720p", DLSSSR_SOURCE_720p);
    ComboBox_AddStringData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS \x2264 1080p", DLSSSR_SOURCE_1080p);
    ComboBox_AddStringData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS \x2264 1440p", DLSSSR_SOURCE_1440p);

	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETRANGEMIN, FALSE, JVR_VERTICAL_MIN);
	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETRANGEMAX, TRUE, JVR_VERTICAL_MAX);
	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETTIC, 0, JVR_VERTICAL_DEFAULT);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETRANGEMIN, FALSE, JVR_REPROJECTION_MIN);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETRANGEMAX, TRUE, JVR_REPROJECTION_MAX);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETTIC, 0, JVR_REPROJECTION_DEFAULT);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETTIC, 0, 100);
	ComboBox_AddStringData(m_hWnd, IDC_JVR_EYE, L"Left eye", 0);
	ComboBox_AddStringData(m_hWnd, IDC_JVR_EYE, L"Right eye", 1);
	ComboBox_AddStringData(m_hWnd, IDC_MFG_PROCESSING_MODE,
		L"Upscale then MFG", 0);
	ComboBox_AddStringData(m_hWnd, IDC_MFG_PROCESSING_MODE,
		L"MFG then upscale", 1);

	ComboBox_AddStringData(m_hWnd, IDC_EXTENDED_STARTUP, L"Play first", 0);
	ComboBox_AddStringData(m_hWnd, IDC_EXTENDED_STARTUP, L"Wait for Extended", 1);

	ComboBox_AddStringData(m_hWnd, IDC_ZIPDEPTH_MODE, L"Both", ZIPDEPTH_Both);
	ComboBox_AddStringData(m_hWnd, IDC_ZIPDEPTH_MODE, L"DLSS SR Only", ZIPDEPTH_DlssSrOnly);
	ComboBox_AddStringData(m_hWnd, IDC_ZIPDEPTH_MODE, L"MFG Only", ZIPDEPTH_MfgOnly);
	ComboBox_AddStringData(m_hWnd, IDC_ZIPDEPTH_MODE, L"Disable", ZIPDEPTH_Disable);

	// The public MFG setting is now a checkbox. The internal Auto cadence
	// algorithm remains unchanged (Off=0, Auto=7).
	if (!::GetDlgItem(m_hWnd, IDC_MFG_MULTIPLIER)) return E_FAIL;
	return S_OK;
}

void CVRMainPPage::AddEditionHints()
{
	AddHint(IDC_DLSS_SR_SOURCE_LIMIT,
        L"Apply DLSS only at or below the selected source resolution (same limits as RTX VSR).\n"
        L"Target is always the display-fit resolution. DLAA is used at 1:1; otherwise DLSS SR.\n"
        L"Forward/backward Optical Flow validation is requested automatically.\n"
        L"Requires NVIDIA RTX, 8-bit SDR and edition\\nvngx_dlss.dll. The processing order controls the MFG slot.");
	AddHint(IDC_MFG_PROCESSING_MODE,
		L"Choose whether to upscale before or after MFG. Both orders support RTX VSR and DLSS SR.\n"
		L"After-MFG upscaling uses the native-size path when the video and output support it.\n"
		L"The new order applies immediately. Pending original frames are presented before the route changes; a brief pause is possible.");
	AddHint(IDC_EXTENDED_STARTUP,
		L"Play first: Show video while Extended features load.\n"
		L"Wait for Extended: Prepare enabled features before playback.\n"
		L"Applies when you open a video.");
	AddHint(IDC_ZIPDEPTH_MODE,
		L"Choose which Extended path receives ZipDepth.\n"
		L"Both (default), DLSS SR Only, MFG Only, or Disable.");
    AddHint(IDC_VIDEO_ENHANCEMENT,
        L"Adaptive SDR colour enhancement: temporal SVDLUT, adaptive luma/chroma, "
        L"preferred colours and skin tones. Full calibrated strength. "
        L"Preview live; saved on Apply. Existing cached depth is optional.");
	AddHint(IDC_SHOW_DEPTH_OVERLAY,
		L"Show the active ZipDepth map as a diagnostic overlay.");
	AddHint(IDC_SHOW_MFG_MOTION,
		L"Draw MFG motion arrows on the final picture.\n"
		L"Arrows point from the current source frame to the previous source frame.");
	AddHint(IDC_MFG_FEATURE_BADGES,
		L"Show DLSS SR, VSR and MFG status.\n"
		L"Keep it visible while paused. Show it for 5 seconds after starting, resuming or resizing.");
	AddHint(IDC_EDITION_STATS_DETAILS, L"Show Edition playback diagnostics when Show statistics is enabled.");
	AddHint(IDC_MFG_MULTIPLIER,
		L"Enable automatic DLSS Multi Frame Generation (Auto).\n"
		L"Requires Direct3D 11, compatible NVIDIA RTX hardware and drivers.");
	AddHint(IDC_JVR_FLAT_VIEW,
		L"Show one eye of progressive SDR Full SBS video using a 16:9 stereographic view.");
	AddHint(IDC_JVR_VERTICAL,
		L"Move the centre of the 16:9 view. Negative looks up; positive looks down.");
	AddHint(IDC_JVR_REPROJECTION,
		L"0% shifts the framing after projection. 100% uses camera pitch alone.\n"
		L"Above 100%, additional pitch is offset by a shift in the opposite direction.\n"
		L"The selected centre stays in the same place.");

}

HRESULT CVRMainPPage::ReadEditionControls()
{
	// This is the only settings page and owns the MFG field.  Read the
	// actual control on Apply, including when Windows did not send a selection
	// notification.  Reject a missing selection instead of interpreting -1 as
	// a multiplier or overwriting another saved preference.
	m_SetsPP.bMfgLowMotion = false;
	m_SetsPP.bAdaptiveMfgDepth = false;
	m_SetsPP.bDepth3DPreview = false;
	if (m_showJvrOptions) {
		m_SetsPP.bJvrFlatView = IsDlgButtonChecked(IDC_JVR_FLAT_VIEW) == BST_CHECKED;
		m_SetsPP.iJvrVertical = static_cast<int>(SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_GETPOS));
		m_SetsPP.iJvrReprojection = static_cast<int>(SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_GETPOS));
		const LONG_PTR jvrEye = ComboBox_GetCurItemData(m_hWnd, IDC_JVR_EYE);
		if (jvrEye != CB_ERR) m_SetsPP.iJvrEye = static_cast<int>(jvrEye);
	}
	const LRESULT zipDepthMode = ComboBox_GetCurItemData(m_hWnd, IDC_ZIPDEPTH_MODE);
	if (zipDepthMode < ZIPDEPTH_Both || zipDepthMode > ZIPDEPTH_Disable) return E_INVALIDARG;
	edition::ApplyZipDepthMode(m_SetsPP, static_cast<int>(zipDepthMode));
	m_SetsPP.bShowMfgMotion = IsDlgButtonChecked(IDC_SHOW_MFG_MOTION) == BST_CHECKED;
	m_SetsPP.bShowDepthOverlay = IsDlgButtonChecked(IDC_SHOW_DEPTH_OVERLAY) == BST_CHECKED;
    // Retired A strength is kept only for Settings_t ABI / old preset compatibility.
    m_SetsPP.SetSvdLutStrength(0);
    if (m_videoEnhancement != (IsDlgButtonChecked(IDC_VIDEO_ENHANCEMENT) == BST_CHECKED))
        return E_FAIL;
	m_SetsPP.bShowFeatureBadges = IsDlgButtonChecked(IDC_MFG_FEATURE_BADGES) == BST_CHECKED;
	m_SetsPP.bShowEditionDetails = IsDlgButtonChecked(IDC_EDITION_STATS_DETAILS) == BST_CHECKED;
    const LRESULT srLimit = ComboBox_GetCurItemData(m_hWnd, IDC_DLSS_SR_SOURCE_LIMIT);
    if (srLimit < DLSSSR_SOURCE_Disable || srLimit > DLSSSR_SOURCE_1440p) return E_INVALIDARG;
    m_SetsPP.iDlssSRSourceLimit = static_cast<int>(srLimit);
    m_SetsPP.bDlssSR = srLimit != DLSSSR_SOURCE_Disable;
    m_SetsPP.bDlssSRDlaa = m_SetsPP.bDlssSR;
	const UINT mfgState = IsDlgButtonChecked(IDC_MFG_MULTIPLIER);
	if (mfgState != BST_CHECKED && mfgState != BST_UNCHECKED) return E_INVALIDARG;
	m_SetsPP.iMfgMultiplier = mfgState == BST_CHECKED ? 7u : 0u;
	const LRESULT processing = ComboBox_GetCurItemData(m_hWnd, IDC_MFG_PROCESSING_MODE);
	if (processing != 0 && processing != 1) return E_INVALIDARG;
	m_SetsPP.iMfgProcessingMode = static_cast<unsigned>(processing);
	const LRESULT startup = ComboBox_GetCurItemData(m_hWnd, IDC_EXTENDED_STARTUP);
	if (startup != 0 && startup != 1) return E_INVALIDARG;
	m_SetsPP.bWaitForExtendedStartup = startup == 1;
	return S_OK;
}

void CVRMainPPage::CommitPreviewSettings()
{
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
    m_oldVideoEnhancement = m_videoEnhancement;
	m_oldJvrFlatView = m_SetsPP.bJvrFlatView;
	m_oldJvrVertical = m_SetsPP.iJvrVertical;
	m_oldJvrReprojection = m_SetsPP.iJvrReprojection;
	m_oldJvrEye = m_SetsPP.iJvrEye;

}

bool CVRMainPPage::OnEditionMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_COMMAND) {
		LRESULT lValue;
		const int nID = LOWORD(wParam);
		const int action = HIWORD(wParam);
        if (action == BN_CLICKED && nID == IDC_VIDEO_ENHANCEMENT) {
            const bool checked = IsDlgButtonChecked(nID) == BST_CHECKED;
            if (checked != m_videoEnhancement && PreviewVideoEnhancement(checked)) SetDirty();
            CheckDlgButton(nID, m_videoEnhancement ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
		if (action == CBN_SELCHANGE && nID == IDC_JVR_EYE) {
			const LONG_PTR value = ComboBox_GetCurItemData(m_hWnd, nID);
			if (value != CB_ERR) {
				m_SetsPP.iJvrEye = static_cast<int>(value);
				ApplyJvrPreview();
				SetDirty();
			}
			return TRUE;
		}

		if (action == BN_CLICKED) {
			if (nID == IDC_EDITION_STATS_DETAILS) {
				m_SetsPP.bShowEditionDetails = IsDlgButtonChecked(nID) == BST_CHECKED;
				SetDirty();
				return TRUE;
			}
			if (nID == IDC_JVR_FLAT_VIEW) {
				m_SetsPP.bJvrFlatView = IsDlgButtonChecked(IDC_JVR_FLAT_VIEW) == BST_CHECKED;
				ApplyJvrPreview();
				EnableControls();
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_SHOW_DEPTH_OVERLAY) {
				m_SetsPP.bShowDepthOverlay = IsDlgButtonChecked(IDC_SHOW_DEPTH_OVERLAY) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_SHOW_MFG_MOTION) {
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_MFG_FEATURE_BADGES) {
				m_SetsPP.bShowFeatureBadges = IsDlgButtonChecked(IDC_MFG_FEATURE_BADGES) == BST_CHECKED;
				SetDirty();
				return (LRESULT)1;
			}
			if (nID == IDC_MFG_MULTIPLIER) {
				const unsigned requested = IsDlgButtonChecked(IDC_MFG_MULTIPLIER) == BST_CHECKED ? 7u : 0u;
				if (m_SetsPP.iMfgMultiplier != requested) {
					m_SetsPP.iMfgMultiplier = requested;
					EnableControls();
					SetDirty();
				}
				return (LRESULT)1;
			}
		}
		if (action == CBN_SELCHANGE) {
			if (nID == IDC_ZIPDEPTH_MODE) {
				const LRESULT mode = ComboBox_GetCurItemData(m_hWnd, nID);
				if (mode >= ZIPDEPTH_Both && mode <= ZIPDEPTH_Disable) {
					edition::ApplyZipDepthMode(m_SetsPP, static_cast<int>(mode));
					EnableControls();
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_EXTENDED_STARTUP) {
				lValue = ComboBox_GetCurItemData(m_hWnd, nID);
				if ((lValue == 0 || lValue == 1) && m_SetsPP.bWaitForExtendedStartup != (lValue == 1)) {
					m_SetsPP.bWaitForExtendedStartup = lValue == 1;
					SetDirty();
				}
				return (LRESULT)1;
			}
            if (nID == IDC_DLSS_SR_SOURCE_LIMIT) {
                const LRESULT choice = ComboBox_GetCurItemData(m_hWnd, nID);
                if (choice >= DLSSSR_SOURCE_Disable && choice <= DLSSSR_SOURCE_1440p &&
                    m_SetsPP.iDlssSRSourceLimit != choice) {
                    m_SetsPP.iDlssSRSourceLimit = static_cast<int>(choice);
                    m_SetsPP.bDlssSR = choice != DLSSSR_SOURCE_Disable;
                    m_SetsPP.bDlssSRDlaa = m_SetsPP.bDlssSR;
                    EnableControls();
                    SetDirty();
                }
                return (LRESULT)1;
            }
			if (nID == IDC_MFG_PROCESSING_MODE) {
				lValue = ComboBox_GetCurItemData(m_hWnd, nID);
				if ((lValue == 0 || lValue == 1) && m_SetsPP.iMfgProcessingMode != static_cast<unsigned>(lValue)) {
					m_SetsPP.iMfgProcessingMode = static_cast<unsigned>(lValue);
					SetDirty();
				}
				return (LRESULT)1;
			}
		}
	} else if (uMsg == WM_HSCROLL) {
		if ((HWND)lParam == GetDlgItem(IDC_JVR_VERTICAL) || (HWND)lParam == GetDlgItem(IDC_JVR_REPROJECTION)) {
			m_SetsPP.iJvrVertical = static_cast<int>(SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_GETPOS));
			m_SetsPP.iJvrReprojection = static_cast<int>(SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_GETPOS));
			UpdateJvrViewLabels();
			ApplyJvrPreview();
			SetDirty();
			return (LRESULT)1;
		}
	}
	return false;
}

