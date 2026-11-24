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

void CVRMainPPage::UpdateSvdLutLabel()
{
    SetDlgItemTextW(IDC_SVDLUT_VALUE,
        std::format(L"A: {}%", m_SetsPP.GetSvdLutStrength()).c_str());
    SetDlgItemTextW(IDC_SVDLUT_B_VALUE,
        std::format(L"B: {}%", m_svdLutBStrength).c_str());
}

void CVRMainPPage::PreviewSvdLutStrength(int strength)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && config->Flt_SetInt("svdLutStrengthPreview", strength) == S_OK)
        m_svdLutPreviewed = true;
}

void CVRMainPPage::PreviewSvdLutBStrength(int strength)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && config->Flt_SetInt("svdLutBStrengthPreview", strength) == S_OK)
        m_svdLutBPreviewed = true;
}

void CVRMainPPage::PreviewSvdLutOff(bool off)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && SUCCEEDED(config->Flt_SetBool("svdLutOff", off))) m_svdLutOff = off;
}

void CVRMainPPage::PreviewSvdLutRaw(bool raw)
{
	CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
	if (config && SUCCEEDED(config->Flt_SetBool("svdLutRaw", raw))) m_svdLutRaw = raw;
}

void CVRMainPPage::PreviewSvdLutTemporal(bool temporal)
{
	CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
	if (config && SUCCEEDED(config->Flt_SetBool("svdLutTemporal", temporal))) m_svdLutTemporal = temporal;
}

void CVRMainPPage::PreviewSvdLutB2Luminance(bool enable)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && SUCCEEDED(config->Flt_SetBool("svdLutB2Luminance", enable)))
        m_svdLutB2Luminance = enable;
}

void CVRMainPPage::PreviewSvdLutB2Memory(bool enable)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && SUCCEEDED(config->Flt_SetBool("svdLutB2Memory", enable)))
        m_svdLutB2Memory = enable;
}

void CVRMainPPage::PreviewSvdLutR1Gamut(bool enable)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && SUCCEEDED(config->Flt_SetBool("svdLutR1Gamut", enable)))
        m_svdLutR1Gamut = enable;
}

void CVRMainPPage::PreviewSkinTones(bool enable)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && SUCCEEDED(config->Flt_SetBool("skinTones", enable)))
        m_skinTones = enable;
}

void CVRMainPPage::PreviewPreferredColors(bool enable)
{
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if (config && SUCCEEDED(config->Flt_SetBool("preferredColors", enable)))
        m_preferredColors = enable;
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
	SendDlgItemMessageW(IDC_SVDLUT_STRENGTH, TBM_SETPOS, TRUE, m_SetsPP.GetSvdLutStrength());
    SendDlgItemMessageW(IDC_SVDLUT_B_STRENGTH, TBM_SETPOS, TRUE, m_svdLutBStrength);
	ComboBox_SelectByItemData(m_hWnd, IDC_SVDLUT_MODE,
        m_svdLutOff ? 0 : (m_svdLutRaw ? (m_svdLutTemporal ? 3 : 2) : 1));
	UpdateSvdLutLabel();
    CheckDlgButton(IDC_SVDLUT_B2_LUMINANCE, m_svdLutB2Luminance ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SVDLUT_B2_MEMORY, m_svdLutB2Memory ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SVDLUT_R1_GAMUT, m_svdLutR1Gamut ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SKIN_TONES, m_skinTones ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_PREFERRED_COLORS, m_preferredColors ? BST_CHECKED : BST_UNCHECKED);
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
	GetDlgItem(IDC_SVDLUT_STRENGTH).EnableWindow(canUseDlssSR);
	GetDlgItem(IDC_SVDLUT_VALUE).EnableWindow(canUseDlssSR);
    GetDlgItem(IDC_SVDLUT_B_STRENGTH).EnableWindow(canUseDlssSR);
    GetDlgItem(IDC_SVDLUT_B_VALUE).EnableWindow(canUseDlssSR);
	GetDlgItem(IDC_SVDLUT_MODE).EnableWindow(canUseDlssSR);
    const bool b2Available = canUseDlssSR && m_svdLutRaw && m_svdLutTemporal && !m_svdLutOff;
    GetDlgItem(IDC_SVDLUT_B2_LUMINANCE).EnableWindow(b2Available);
    GetDlgItem(IDC_SVDLUT_B2_MEMORY).EnableWindow(b2Available);
    GetDlgItem(IDC_SVDLUT_R1_GAMUT).EnableWindow(b2Available);
    // Enhancements are independent of SVDLUT mode and legacy B2.2/R1.
    GetDlgItem(IDC_SKIN_TONES).EnableWindow(canUseDlssSR);
    GetDlgItem(IDC_PREFERRED_COLORS).EnableWindow(canUseDlssSR);
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
	const bool restoreSvdLut = m_svdLutPreviewed || m_svdLutBPreviewed || m_svdLutOff != m_oldSvdLutOff ||
        m_svdLutRaw != m_oldSvdLutRaw ||
		m_svdLutTemporal != m_oldSvdLutTemporal ||
        m_svdLutB2Luminance != m_oldSvdLutB2Luminance ||
        m_svdLutB2Memory != m_oldSvdLutB2Memory ||
        m_svdLutR1Gamut != m_oldSvdLutR1Gamut ||
        m_skinTones != m_oldSkinTones ||
        m_preferredColors != m_oldPreferredColors;
	if (restoreSettings) {
		// Roll back the fields that can be previewed before Apply.
		live.iSDRDisplayNits = m_oldSDRDisplayNits;
		live.bJvrFlatView = m_oldJvrFlatView;
		live.iJvrVertical = m_oldJvrVertical;
		live.iJvrReprojection = m_oldJvrReprojection;
		live.iJvrEye = m_oldJvrEye;
		m_pVideoRenderer->SetSettings(live);
	}

	if (m_svdLutPreviewed) {
		PreviewSvdLutStrength(m_oldSvdLutStrength);
		m_svdLutPreviewed = false;
	}
    if (m_svdLutBPreviewed) {
        PreviewSvdLutBStrength(m_oldSvdLutBStrength);
        m_svdLutBPreviewed = false;
        m_svdLutBStrength = m_oldSvdLutBStrength;
    }
	if (m_svdLutTemporal != m_oldSvdLutTemporal) PreviewSvdLutTemporal(m_oldSvdLutTemporal);
    if (m_svdLutB2Luminance != m_oldSvdLutB2Luminance)
        PreviewSvdLutB2Luminance(m_oldSvdLutB2Luminance);
    if (m_svdLutB2Memory != m_oldSvdLutB2Memory)
        PreviewSvdLutB2Memory(m_oldSvdLutB2Memory);
    if (m_svdLutR1Gamut != m_oldSvdLutR1Gamut)
        PreviewSvdLutR1Gamut(m_oldSvdLutR1Gamut);
    if (m_skinTones != m_oldSkinTones)
        PreviewSkinTones(m_oldSkinTones);
    if (m_preferredColors != m_oldPreferredColors)
        PreviewPreferredColors(m_oldPreferredColors);
	if (m_svdLutRaw != m_oldSvdLutRaw) PreviewSvdLutRaw(m_oldSvdLutRaw);
    if (m_svdLutOff != m_oldSvdLutOff) PreviewSvdLutOff(m_oldSvdLutOff);
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
	if (!m_svdLutPreviewed) m_oldSvdLutStrength = m_SetsPP.GetSvdLutStrength();
    CComQIPtr<IExFilterConfig> bConfig(m_pVideoRenderer.p);
    int liveB = 200;
    if (bConfig && SUCCEEDED(bConfig->Flt_GetInt("svdLutBStrength", &liveB)))
        m_svdLutBStrength = std::clamp(liveB, 0, 200);
    m_oldSvdLutBStrength = m_svdLutBStrength;
    m_svdLutBPreviewed = false;
	CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
	m_svdLutOff = false;
    if (config) config->Flt_GetBool("svdLutOff", &m_svdLutOff);
    m_oldSvdLutOff = m_svdLutOff;
	m_svdLutRaw = false;
	if (config) config->Flt_GetBool("svdLutRaw", &m_svdLutRaw);
	m_oldSvdLutRaw = m_svdLutRaw;
	m_svdLutTemporal = false;
	if (config) config->Flt_GetBool("svdLutTemporal", &m_svdLutTemporal);
	m_oldSvdLutTemporal = m_svdLutTemporal;
    m_svdLutB2Luminance = false;
    if (config) config->Flt_GetBool("svdLutB2Luminance", &m_svdLutB2Luminance);
    m_oldSvdLutB2Luminance = m_svdLutB2Luminance;
    m_svdLutB2Memory = false;
    if (config) config->Flt_GetBool("svdLutB2Memory", &m_svdLutB2Memory);
    m_oldSvdLutB2Memory = m_svdLutB2Memory;
    m_svdLutR1Gamut = false;
    if (config) config->Flt_GetBool("svdLutR1Gamut", &m_svdLutR1Gamut);
    m_oldSvdLutR1Gamut = m_svdLutR1Gamut;
    m_skinTones = false;
    if (config) config->Flt_GetBool("skinTones", &m_skinTones);
    m_oldSkinTones = m_skinTones;
    m_preferredColors = false;
    if (config) config->Flt_GetBool("preferredColors", &m_preferredColors);
    m_oldPreferredColors = m_preferredColors;
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

    ComboBox_AddStringData(m_hWnd, IDC_SVDLUT_MODE, L"Off", 0);
    ComboBox_AddStringData(m_hWnd, IDC_SVDLUT_MODE, L"Normal A", 1);
    ComboBox_AddStringData(m_hWnd, IDC_SVDLUT_MODE, L"RAW B", 2);
    ComboBox_AddStringData(m_hWnd, IDC_SVDLUT_MODE, L"Temporal B1", 3);
	SendDlgItemMessageW(IDC_SVDLUT_STRENGTH, TBM_SETRANGE, TRUE, MAKELONG(0, SVDLUT_STRENGTH_MAX));
    SendDlgItemMessageW(IDC_SVDLUT_B_STRENGTH, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
	SendDlgItemMessageW(IDC_SVDLUT_STRENGTH, TBM_SETTIC, 0, SVDLUT_STRENGTH_MAX / 2);
	SendDlgItemMessageW(IDC_SVDLUT_STRENGTH, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(IDC_SVDLUT_B_STRENGTH, TBM_SETTIC, 0, 100);
    SendDlgItemMessageW(IDC_SVDLUT_B_STRENGTH, TBM_SETPAGESIZE, 0, 10);
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
    AddHint(IDC_SVDLUT_STRENGTH,
        L"Normal A only: 100% matches the previous full A result; 200% is an experimental extension.\n"
        L"0% disables the A grade. Preferred Colors and Skin Tones are independent.");
    AddHint(IDC_SVDLUT_B_STRENGTH,
        L"RAW/Temporal B only: 200% matches the previous complete B grade.\n"
        L"100% is the gentler daily-use candidate; 0% disables B model grading.\n"
        L"B2.1 and perceptual enhancements keep their separate logic.");
	AddHint(IDC_SVDLUT_MODE,
		L"Off: bypass all SVDLUT processing (saved A strength is kept).\n"
        L"Normal A: original scene-based Edition SVDLUT with its strength slider.\n"
		L"RAW B: independent inference and original model LUT for each displayed frame.\n"
		L"Temporal B1: infer original frames, then filter whole LUT outliers and\n"
		L"blend available past/future LUTs. No A brightness/chroma suppression.\n"
		L"Temporal future frames are opportunistic; unavailable frames are not awaited.\n"
		L"Mode is saved on Apply. RAW B may reduce playback FPS.");
    AddHint(IDC_SVDLUT_B2_MEMORY,
        L"B2.2 + B2.1: limit excessive source-relative chroma changes\n"
        L"with a soft knee at the B2.1 code-luma anchor.\n"
        L"Small changes pass unchanged; near-neutral colours get tighter limits.\n"
        L"Works with Preferred/Skin P1 before their independent enhancement.\n"
        L"B2.1 off: legacy memory/gamut path. R1 or M1 selects legacy repair.\n"
        L"No extra inference, readback or GPU pass. Saved on Apply.");
    AddHint(IDC_SKIN_TONES,
        L"Skin Tones: proactively enhance skin-family colour and chroma.\n"
        L"Independent of SVDLUT A/B, B2.1, B2.2 and R1. Not face detection.\n"
        L"Existing cached ZipDepth is used automatically if safely aligned.\n"
        L"No additional depth inference. Saved on Apply.");
    AddHint(IDC_PREFERRED_COLORS,
        L"Preferred Colors: enhance familiar sky blue and foliage green.\n"
        L"Independent of SVDLUT and Skin Tones. Existing ZipDepth is optional\n"
        L"and used automatically only when reliable; otherwise 2D enhancement.\n"
        L"Same LUT draw when possible. Saved on Apply.");
    AddHint(IDC_SVDLUT_R1_GAMUT,
        L"R1 experiment: Continuously roll off chroma near the SDR boundary.\n"
        L"Preserves the B2.1 brightness anchor and B2.2 memory-colour logic,\n"
        L"but removes two discontinuous threshold jumps. No extra inference,\n"
        L"readback, GPU pass or frame-buffer change. This is NOT a complete\n"
        L"libplacebo/ACES implementation. When both B2.2 and R1 are checked,\n"
        L"R1 takes precedence; uncheck R1 for the selected B2.2 path.\n"
        L"Saved on Apply. Compare Video Buffer before accepting."
    );
    AddHint(IDC_SVDLUT_B2_LUMINANCE,
        L"B2.1: Independently limit SVDLUT code-luma participation after B1.\n"
        L"Source samples and neutral response constrain luma; chroma is retained\n"
        L"within the output gamut. Same-shot relaxation is gradual.\n"
        L"No new inference, readback, or full-resolution pass; optional\n"
        L"analysis is limited under low-buffer pressure. B1 is unchanged.\n"
        L"Saved on Apply; paused redraws preserve the presented result.");
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
	m_SetsPP.SetSvdLutStrength(static_cast<int>(SendDlgItemMessageW(IDC_SVDLUT_STRENGTH, TBM_GETPOS)));
    m_svdLutBStrength = static_cast<int>(SendDlgItemMessageW(IDC_SVDLUT_B_STRENGTH, TBM_GETPOS));
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
	m_oldSvdLutStrength = m_SetsPP.GetSvdLutStrength();
    m_oldSvdLutBStrength = m_svdLutBStrength;
    m_svdLutBPreviewed = false;
	m_svdLutPreviewed = false;
	m_oldSvdLutOff = m_svdLutOff;
	m_oldSvdLutRaw = m_svdLutRaw;
	m_oldSvdLutTemporal = m_svdLutTemporal;
    m_oldSvdLutB2Luminance = m_svdLutB2Luminance;
    m_oldSvdLutB2Memory = m_svdLutB2Memory;
    m_oldSvdLutR1Gamut = m_svdLutR1Gamut;
    m_oldSkinTones = m_skinTones;
    m_oldPreferredColors = m_preferredColors;
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
		if (action == CBN_SELCHANGE && nID == IDC_SVDLUT_MODE) {
			const LONG_PTR selection = ComboBox_GetCurItemData(m_hWnd, nID);
			if (selection >= 0 && selection <= 3) {
                const bool off = selection == 0;
                const bool raw = selection >= 2;
                const bool temporal = selection == 3;
                // Switch Off first when disabling; enable B/A after leaving Off.
                if (off && !m_svdLutOff) PreviewSvdLutOff(true);
                if (m_svdLutTemporal != temporal) PreviewSvdLutTemporal(temporal);
                if (m_svdLutRaw != raw) PreviewSvdLutRaw(raw);
                if (!off && m_svdLutOff) PreviewSvdLutOff(false);
                ComboBox_SelectByItemData(m_hWnd, IDC_SVDLUT_MODE,
                    m_svdLutOff ? 0 : (m_svdLutRaw ? (m_svdLutTemporal ? 3 : 2) : 1));
                UpdateSvdLutLabel();
                EnableEditionControls();
                if (m_svdLutOff != m_oldSvdLutOff || m_svdLutRaw != m_oldSvdLutRaw ||
                    m_svdLutTemporal != m_oldSvdLutTemporal)
                    SetDirty();
            }
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

        if (action == BN_CLICKED && nID == IDC_SVDLUT_B2_MEMORY) {
            const bool checked = IsDlgButtonChecked(nID) == BST_CHECKED;
            if (checked != m_svdLutB2Memory) {
                PreviewSvdLutB2Memory(checked);
                EnableEditionControls();
                SetDirty();
            }
            return TRUE;
        }
        if (action == BN_CLICKED && nID == IDC_SKIN_TONES) {
            const bool checked = IsDlgButtonChecked(nID) == BST_CHECKED;
            if (checked != m_skinTones) {
                PreviewSkinTones(checked);
                EnableEditionControls();
                SetDirty();
            }
            return TRUE;
        }
        if (action == BN_CLICKED && nID == IDC_PREFERRED_COLORS) {
            const bool checked = IsDlgButtonChecked(nID) == BST_CHECKED;
            if (checked != m_preferredColors) {
                PreviewPreferredColors(checked);
                SetDirty();
            }
            return TRUE;
        }
        if (action == BN_CLICKED && nID == IDC_SVDLUT_R1_GAMUT) {
            const bool checked = IsDlgButtonChecked(nID) == BST_CHECKED;
            if (checked != m_svdLutR1Gamut) {
                PreviewSvdLutR1Gamut(checked);
                EnableEditionControls();
                SetDirty();
            }
            return TRUE;
        }
        if (action == BN_CLICKED && nID == IDC_SVDLUT_B2_LUMINANCE) {
            const bool checked = IsDlgButtonChecked(nID) == BST_CHECKED;
            if (checked != m_svdLutB2Luminance) {
                PreviewSvdLutB2Luminance(checked);
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
		if ((HWND)lParam == GetDlgItem(IDC_SVDLUT_STRENGTH)) {
			const int strength = static_cast<int>(SendDlgItemMessageW(IDC_SVDLUT_STRENGTH, TBM_GETPOS));
			if (strength != m_SetsPP.GetSvdLutStrength()) {
				m_SetsPP.SetSvdLutStrength(strength);
				UpdateSvdLutLabel();
				PreviewSvdLutStrength(strength);
				SetDirty();
			}
			return (LRESULT)1;
		}
        if ((HWND)lParam == GetDlgItem(IDC_SVDLUT_B_STRENGTH)) {
            const int strength = static_cast<int>(SendDlgItemMessageW(IDC_SVDLUT_B_STRENGTH, TBM_GETPOS));
            if (strength != m_svdLutBStrength) {
                m_svdLutBStrength = strength;
                UpdateSvdLutLabel();
                PreviewSvdLutBStrength(strength);
                SetDirty();
            }
            return (LRESULT)1;
        }
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

