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
#include <cwchar>

void CVRMainPPage::SetNumericEdit(int id,int value)
{
    const bool prior=m_updatingNumeric;
    m_updatingNumeric=true;
    SetDlgItemTextW(id,std::to_wstring(value).c_str());
    m_updatingNumeric=prior;
}

void CVRMainPPage::UpdateJvrViewLabels()
{
    SetNumericEdit(IDC_JVR_VERTICAL_VALUE,m_SetsPP.iJvrVertical);
    SetNumericEdit(IDC_JVR_REPROJECTION_VALUE,m_SetsPP.iJvrReprojection);
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

namespace {
constexpr int ColorIds[]={IDC_VIDEO_ENHANCEMENT,IDC_MEMORY_COLOR,IDC_SKIN_TONE,IDC_ADAPTIVE_TONE};
constexpr int ColorLabels[]={IDC_COLOR_ENHANCEMENT_VALUE,IDC_MEMORY_COLOR_VALUE,IDC_SKIN_TONE_VALUE,IDC_ADAPTIVE_TONE_VALUE};
constexpr const char* ColorKeys[]={"colorEnhancementStrength","memoryColorStrength","skinToneStrength","adaptiveToneStrength"};
constexpr int ColorNames[]={IDC_COLOR_LABEL,IDC_MEMORY_LABEL,IDC_SKIN_LABEL,IDC_TONE_LABEL};
constexpr int ColorUnits[]={IDC_COLOR_PERCENT,IDC_MEMORY_PERCENT,IDC_SKIN_PERCENT,IDC_TONE_PERCENT};
constexpr int VrIds[]={IDC_VR_GROUP,IDC_JVR_FLAT_VIEW,IDC_JVR_PROFILE_NAME,IDC_JVR_EYE,IDC_JVR_VERTICAL,IDC_JVR_VERTICAL_VALUE,IDC_JVR_REPROJECTION,IDC_JVR_REPROJECTION_VALUE,IDC_VR_VERTICAL_LABEL,IDC_VR_REPROJECTION_LABEL,IDC_VR_VERTICAL_PERCENT,IDC_VR_REPROJECTION_PERCENT};
constexpr int VrAdjustIds[]={IDC_JVR_EYE,IDC_JVR_VERTICAL,IDC_JVR_VERTICAL_VALUE,IDC_JVR_REPROJECTION,IDC_JVR_REPROJECTION_VALUE,IDC_VR_VERTICAL_LABEL,IDC_VR_REPROJECTION_LABEL,IDC_VR_VERTICAL_PERCENT,IDC_VR_REPROJECTION_PERCENT};

}
bool CVRMainPPage::PreviewColorStrength(int index,int value) {
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if(!m_colorCaptured[index] || !config || FAILED(config->Flt_SetInt(ColorKeys[index],value))) return false;
    m_colorStrengths[index]=value;
    return true;
}
void CVRMainPPage::UpdateColorStrengthLabel(int index,int value) {
    SetNumericEdit(ColorLabels[index],value);
}
bool CVRMainPPage::PreviewEnhancementEnabled(bool enabled) {
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if(!m_masterCaptured || !config || FAILED(config->Flt_SetBool("enhancementEnabled",enabled))) return false;
    m_enhancementEnabled=enabled;
    return true;
}
void CVRMainPPage::ResetColorStrengths() {
    for(int i=0;i<4;++i) PreviewColorStrength(i,100);
    PreviewEnhancementEnabled(true);
}
// Finalize on focus loss or Apply, not on every keystroke. Empty/invalid input
// restores the last value; valid integers clamp to the legal control range.
bool CVRMainPPage::CommitNumericEdit(int id) {
    int index=-1;
    for(int i=0;i<4;++i) if(id==ColorLabels[i]) index=i;
    const bool vertical=id==IDC_JVR_VERTICAL_VALUE;
    if(index<0 && !vertical && id!=IDC_JVR_REPROJECTION_VALUE) return true;
    const int old=index>=0?m_colorStrengths[index]:(vertical?m_SetsPP.iJvrVertical:m_SetsPP.iJvrReprojection);
    const int minimum=index>=0?0:(vertical?JVR_VERTICAL_MIN:JVR_REPROJECTION_MIN);
    const int maximum=index>=0?100:(vertical?JVR_VERTICAL_MAX:JVR_REPROJECTION_MAX);
    wchar_t text[32]{};
    ::GetDlgItemTextW(ControlParent(id),id,text,32);
    wchar_t* end=nullptr;
    const long parsed=std::wcstol(text,&end,10);
    const int value=end!=text && *end==L'\0'?static_cast<int>(std::clamp(parsed,long(minimum),long(maximum))):old;
    if(index>=0) {
        if(value!=old && (!m_enhancementEnabled || !m_colorCaptured[index] || !PreviewColorStrength(index,value))) {
            SetNumericEdit(id,old); SendDlgItemMessageW(ColorIds[index],TBM_SETPOS,TRUE,old); return false;
        }
        SendDlgItemMessageW(ColorIds[index],TBM_SETPOS,TRUE,value);
    } else {
        if(vertical) m_SetsPP.iJvrVertical=value; else m_SetsPP.iJvrReprojection=value;
        SendDlgItemMessageW(vertical?IDC_JVR_VERTICAL:IDC_JVR_REPROJECTION,TBM_SETPOS,TRUE,value);
        if(value!=old) ApplyJvrPreview();
    }
    SetNumericEdit(id,value);
    if(value!=old) SetDirty();
    return true;
}
HWND CVRMainPPage::ControlParent(int id) const
{
    // Footer/tab remain on the outer property page. Native and Edition IDs
    // are disjoint; hidden panes must remain accessible during Apply/Default.
    if (m_rendererPane && ::GetDlgItem(m_rendererPane, id)) return m_rendererPane;
    if (m_extendedPane && ::GetDlgItem(m_extendedPane, id)) return m_extendedPane;
    return m_hWnd;
}

INT_PTR CALLBACK CVRMainPPage::SettingsPaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_ERASEBKGND: {
        // A pane shares the same screen area with its sibling. Explicitly
        // erase its background so pixels of the outgoing pane cannot remain
        // in the gaps between controls. Pane resources must NOT use
        // WS_CLIPCHILDREN: BS_GROUPBOX occupies its entire rectangle but
        // relies on the parent to paint its interior. The outer property
        // page still clips its opaque child panes; sibling clipping remains.
        RECT bounds{};
        if (::GetClientRect(hwnd, &bounds)) {
            return ::FillRect(reinterpret_cast<HDC>(wp), &bounds,
                ::GetSysColorBrush(COLOR_3DFACE)) != 0;
        }
        return FALSE;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_3DFACE));
    case WM_COMMAND:
    case WM_HSCROLL:
        // Preserve the original control HWND/ID and one settings transaction.
        return ::SendMessageW(::GetParent(hwnd), msg, wp, lp) != 0;
    case WM_NOTIFY:
        if (const LRESULT result = ::SendMessageW(::GetParent(hwnd), msg, wp, lp)) {
            ::SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, result);
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE; // Native dialog background, theming and keyboard handling.
    }
}

HRESULT CVRMainPPage::InitializeSettingsTabs()
{
    HWND tab = ::GetDlgItem(m_hWnd, IDC_SETTINGS_TAB);
    if (!tab) return E_FAIL;
    TCITEMW item{}; item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(L"Renderer");
    if (::SendMessageW(tab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)) != 0) return E_FAIL;
    item.pszText = const_cast<LPWSTR>(L"Edition Extended");
    if (::SendMessageW(tab, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&item)) != 1) return E_FAIL;
    RECT content{};
    ::GetClientRect(tab, &content);
    ::SendMessageW(tab, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&content));
    ::MapWindowPoints(tab, m_hWnd, reinterpret_cast<POINT*>(&content), 2);
    auto create = [&](int id) -> HWND {
        HWND pane = ::CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(id), m_hWnd, SettingsPaneProc, 0);
        if (pane) {
            ::SetWindowLongPtrW(pane, GWLP_ID, id);
            ::SetWindowPos(pane, HWND_TOP, content.left, content.top,
                content.right - content.left, content.bottom - content.top, SWP_NOACTIVATE);
        }
        return pane;
    };
    m_rendererPane = create(IDD_EDITION_RENDERER_PANE);
    m_extendedPane = create(IDD_EDITION_EXTENDED_PANE);
    if (!m_rendererPane || !m_extendedPane) {
        if (m_rendererPane) ::DestroyWindow(m_rendererPane);
        if (m_extendedPane) ::DestroyWindow(m_extendedPane);
        m_rendererPane = m_extendedPane = nullptr;
        return E_FAIL;
    }
    // ShowSettingsTab raises the selected pane above the native tab.
    // WS_CLIPSIBLINGS keeps tab repaint from covering the child dialogs.
    m_editionTab = false;
    ShowSettingsTab();
    return S_OK;
}

void CVRMainPPage::ShowSettingsTab()
{
    HWND visible = m_editionTab ? m_extendedPane : m_rendererPane;
    HWND hidden = m_editionTab ? m_rendererPane : m_extendedPane;
    if (!visible || !hidden) return;
    const HWND focus = ::GetFocus();
    if (focus == hidden || ::IsChild(hidden, focus)) ::SetFocus(::GetDlgItem(m_hWnd, IDC_SETTINGS_TAB));
    // Do not batch overlapping sibling visibility changes: finish hiding the
    // outgoing pane before exposing the incoming pane, then explicitly paint
    // the newly exposed client area and its existing controls.
    ::ShowWindow(hidden, SW_HIDE);
    if (!::SetWindowPos(visible, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        ::ShowWindow(visible, SW_SHOWNA);
    }
    ::RedrawWindow(visible, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

HRESULT CVRMainPPage::OnDeactivate()
{
    m_bActivated = false;
    if (m_hHint) ::DestroyWindow(m_hHint);
    m_hHint = nullptr;
    // The parent dialog destroys both children. The preview checkpoint lives
    // on the property page object and must survive a Deactivate/Activate pair.
    m_rendererPane = m_extendedPane = nullptr;
    return S_OK;
}
void CVRMainPPage::SetEditionControls()
{
	CheckDlgButton(IDC_JVR_FLAT_VIEW, m_SetsPP.bJvrFlatView ? BST_CHECKED : BST_UNCHECKED);
	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETPOS, TRUE, m_SetsPP.iJvrVertical);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETPOS, TRUE, m_SetsPP.iJvrReprojection);
	ComboBox_SelectByItemData(m_extendedPane, IDC_JVR_EYE, m_SetsPP.iJvrEye);
	UpdateJvrViewLabels();

	CheckDlgButton(IDC_MFG_MULTIPLIER, m_SetsPP.iMfgMultiplier >= 2 ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SelectByItemData(m_extendedPane, IDC_DEPTH_MODE, edition::DepthModeFromSettings(m_SetsPP));
	CheckDlgButton(IDC_SHOW_MFG_MOTION, m_SetsPP.bShowMfgMotion ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_ENHANCEMENT_MASTER,m_enhancementEnabled?BST_CHECKED:BST_UNCHECKED);
    for(int i=0;i<4;++i) {
        SendDlgItemMessageW(ColorIds[i],TBM_SETPOS,TRUE,m_colorStrengths[i]);
        UpdateColorStrengthLabel(i,m_colorStrengths[i]);
    }
	CheckDlgButton(IDC_SHOW_DEPTH_OVERLAY, m_SetsPP.bShowDepthOverlay ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_MFG_FEATURE_BADGES, m_SetsPP.bShowFeatureBadges ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_EDITION_STATS_DETAILS, m_SetsPP.bShowEditionDetails ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SelectByItemData(m_extendedPane, IDC_MFG_PROCESSING_MODE, m_SetsPP.iMfgProcessingMode);
	ComboBox_SelectByItemData(m_extendedPane, IDC_EXTENDED_STARTUP, m_SetsPP.bWaitForExtendedStartup);

    ComboBox_SelectByItemData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT, m_SetsPP.iDlssSRSourceLimit);
}

void CVRMainPPage::EnableEditionControls()
{
	for (const int id : VrIds) {
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
	for (const int id : VrAdjustIds) {
		GetDlgItem(id).EnableWindow(jvrEnabled);
	}
	const BOOL bMfgEnabled = m_SetsPP.bUseD3D11 && m_SetsPP.iMfgMultiplier >= 2 && IsWindows10OrGreater();
	GetDlgItem(IDC_MFG_MULTIPLIER).EnableWindow(m_SetsPP.bUseD3D11 && IsWindows10OrGreater());
	GetDlgItem(IDC_MFG_PROCESSING_MODE).EnableWindow(bMfgEnabled);
	const BOOL canUseDlssSR = m_SetsPP.bUseD3D11 && IsWindows10OrGreater();
	GetDlgItem(IDC_MFG_FEATURE_BADGES).EnableWindow(m_SetsPP.bUseD3D11);
	GetDlgItem(IDC_EDITION_STATS_DETAILS).EnableWindow(m_SetsPP.bUseD3D11 && m_SetsPP.bShowFeatureBadges);
	const bool srEnabled = canUseDlssSR && m_SetsPP.bDlssSR;
	GetDlgItem(IDC_DEPTH_MODE).EnableWindow(canUseDlssSR);
    GetDlgItem(IDC_ENHANCEMENT_MASTER).EnableWindow(canUseDlssSR && m_masterCaptured);
    for(int i=0;i<4;++i) {
        const bool enabled=canUseDlssSR && m_enhancementEnabled && m_colorCaptured[i];
        for(int id:{ColorIds[i],ColorLabels[i],ColorNames[i],ColorUnits[i]}) GetDlgItem(id).EnableWindow(enabled);
    }
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
    bool restoreColor=false;
	if (restoreSettings) {
		// Roll back the fields that can be previewed before Apply.
		live.iSDRDisplayNits = m_oldSDRDisplayNits;
		live.bJvrFlatView = m_oldJvrFlatView;
		live.iJvrVertical = m_oldJvrVertical;
		live.iJvrReprojection = m_oldJvrReprojection;
		live.iJvrEye = m_oldJvrEye;
		m_pVideoRenderer->SetSettings(live);
	}

    // If restoring an off master, turn it off first; if restoring on, restore values first.
    if(m_masterCaptured && !m_oldEnhancementEnabled && m_enhancementEnabled) {
        restoreColor=true; PreviewEnhancementEnabled(false);
    }
    for(int i=0;i<4;++i) {
        if(m_colorCaptured[i] && m_colorStrengths[i]!=m_oldColorStrengths[i]) {
            restoreColor=true;
            PreviewColorStrength(i,m_oldColorStrengths[i]);
        }
        m_colorCaptured[i]=false;
    }
    if(m_masterCaptured && m_enhancementEnabled!=m_oldEnhancementEnabled) {
        restoreColor=true; PreviewEnhancementEnabled(m_oldEnhancementEnabled);
    }
    m_masterCaptured=false;
	if (trace.Enabled()) {
		const auto finished = GetPreciseTick();
		trace.Record(restoreSettings || restoreColor ? "property_page_close_restore" : "property_page_close_unchanged",
			finished, -1, -1, 0, 0, 0, 0, 0, false, 0, S_OK, finished - started);
	}
}

void CVRMainPPage::CapturePreviewSettings()
{
	m_showJvrOptions = !jvr::ProfileFiles(g_hInst).empty();
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
    CComQIPtr<IExFilterConfig> config(m_pVideoRenderer.p);
    if(!m_masterCaptured && config && SUCCEEDED(config->Flt_GetBool("enhancementEnabled",&m_enhancementEnabled))) {
        m_oldEnhancementEnabled=m_enhancementEnabled;
        m_masterCaptured=true;
    }
    for(int i=0;i<4;++i) {
        if(!m_colorCaptured[i] && config && SUCCEEDED(config->Flt_GetInt(ColorKeys[i],&m_colorStrengths[i]))) {
            m_oldColorStrengths[i]=m_colorStrengths[i];
            m_colorCaptured[i]=true;
        }
    }
	m_oldJvrFlatView = m_SetsPP.bJvrFlatView;
	m_oldJvrVertical = m_SetsPP.iJvrVertical;
	m_oldJvrReprojection = m_SetsPP.iJvrReprojection;
	m_oldJvrEye = m_SetsPP.iJvrEye;

}

HRESULT CVRMainPPage::InitializeEditionControls()
{
    for(int i=0;i<4;++i) {
        SendDlgItemMessageW(ColorIds[i],TBM_SETRANGE,TRUE,MAKELONG(0,100));
        SendDlgItemMessageW(ColorIds[i],TBM_SETLINESIZE,0,1);
        SendDlgItemMessageW(ColorIds[i],TBM_SETPAGESIZE,0,5);
        SendDlgItemMessageW(ColorLabels[i],EM_SETLIMITTEXT,10,0);
    }
    for(int id:{IDC_JVR_VERTICAL_VALUE,IDC_JVR_REPROJECTION_VALUE}) SendDlgItemMessageW(id,EM_SETLIMITTEXT,10,0);
    for(int id:{IDC_JVR_VERTICAL,IDC_JVR_REPROJECTION}) {
        SendDlgItemMessageW(id,TBM_SETLINESIZE,0,1);
        SendDlgItemMessageW(id,TBM_SETPAGESIZE,0,5);
    }
    ComboBox_AddStringData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT, L"Disable", DLSSSR_SOURCE_Disable);
    ComboBox_AddStringData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS for SD", DLSSSR_SOURCE_SD);
    ComboBox_AddStringData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS \x2264 720p", DLSSSR_SOURCE_720p);
    ComboBox_AddStringData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS \x2264 1080p", DLSSSR_SOURCE_1080p);
    ComboBox_AddStringData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT, L"DLSS \x2264 1440p", DLSSSR_SOURCE_1440p);

	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETRANGEMIN, FALSE, JVR_VERTICAL_MIN);
	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETRANGEMAX, TRUE, JVR_VERTICAL_MAX);
	SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_SETTIC, 0, JVR_VERTICAL_DEFAULT);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETRANGEMIN, FALSE, JVR_REPROJECTION_MIN);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETRANGEMAX, TRUE, JVR_REPROJECTION_MAX);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETTIC, 0, JVR_REPROJECTION_DEFAULT);
	SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_SETTIC, 0, 100);
	ComboBox_AddStringData(m_extendedPane, IDC_JVR_EYE, L"Left eye", 0);
	ComboBox_AddStringData(m_extendedPane, IDC_JVR_EYE, L"Right eye", 1);
	ComboBox_AddStringData(m_extendedPane, IDC_MFG_PROCESSING_MODE,
		L"Upscale then MFG", 0);
	ComboBox_AddStringData(m_extendedPane, IDC_MFG_PROCESSING_MODE,
		L"MFG then upscale", 1);

	ComboBox_AddStringData(m_extendedPane, IDC_EXTENDED_STARTUP, L"Play first", 0);
	ComboBox_AddStringData(m_extendedPane, IDC_EXTENDED_STARTUP, L"Wait for Extended", 1);

	ComboBox_AddStringData(m_extendedPane, IDC_DEPTH_MODE, L"Both", ZIPDEPTH_Both);
	ComboBox_AddStringData(m_extendedPane, IDC_DEPTH_MODE, L"DLSS SR Only", ZIPDEPTH_DlssSrOnly);
	ComboBox_AddStringData(m_extendedPane, IDC_DEPTH_MODE, L"MFG Only", ZIPDEPTH_MfgOnly);
	ComboBox_AddStringData(m_extendedPane, IDC_DEPTH_MODE, L"Disable", ZIPDEPTH_Disable);

	// The public MFG setting is now a checkbox. The internal Auto cadence
	// algorithm remains unchanged (Off=0, Auto=7).
	if (!::GetDlgItem(m_extendedPane, IDC_MFG_MULTIPLIER)) return E_FAIL;
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
	AddHint(IDC_DEPTH_MODE,
		L"Depth estimation is disabled by default.\n"
		L"For non-commercial scientific research or academic development, choose where to use the model.\n"
		L"See RESEARCH-ONLY.txt and LICENSE-Apple-DepthPro2.txt in the package.");
    AddHint(IDC_ENHANCEMENT_MASTER,L"Enable or bypass all four video enhancements without changing their saved strengths.");
    AddHint(IDC_VIDEO_ENHANCEMENT,L"SDR calibrated SVDLUT color grade. 0% off; 100% full calibrated effect.");
    AddHint(IDC_MEMORY_COLOR,L"Independent SDR preferred sky and foliage colors.");
    AddHint(IDC_SKIN_TONE,L"Independent SDR skin tone enhancement.");
    AddHint(IDC_ADAPTIVE_TONE,L"Independent SDR adaptive shot contrast.");
	AddHint(IDC_SHOW_DEPTH_OVERLAY,
		L"Show the active depth map as a diagnostic overlay.");
	AddHint(IDC_SHOW_MFG_MOTION,
		L"Draw MFG motion arrows on the final picture.\n"
		L"Arrows point from the current source frame to the previous source frame.");
	AddHint(IDC_MFG_FEATURE_BADGES,
		L"Add Edition information to the on-screen statistics.\n"
		L"When statistics are hidden, briefly show active DLSS SR, VSR and MFG as a corner badge.");
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
        CommitNumericEdit(IDC_JVR_VERTICAL_VALUE);
        CommitNumericEdit(IDC_JVR_REPROJECTION_VALUE);
		m_SetsPP.bJvrFlatView = IsDlgButtonChecked(IDC_JVR_FLAT_VIEW) == BST_CHECKED;
		m_SetsPP.iJvrVertical = static_cast<int>(SendDlgItemMessageW(IDC_JVR_VERTICAL, TBM_GETPOS));
		m_SetsPP.iJvrReprojection = static_cast<int>(SendDlgItemMessageW(IDC_JVR_REPROJECTION, TBM_GETPOS));
		const LONG_PTR jvrEye = ComboBox_GetCurItemData(m_extendedPane, IDC_JVR_EYE);
		if (jvrEye != CB_ERR) m_SetsPP.iJvrEye = static_cast<int>(jvrEye);
	}
	const LRESULT depthMode = ComboBox_GetCurItemData(m_extendedPane, IDC_DEPTH_MODE);
	if (depthMode < ZIPDEPTH_Both || depthMode > ZIPDEPTH_Disable) return E_INVALIDARG;
	edition::ApplyDepthMode(m_SetsPP, static_cast<int>(depthMode));
	m_SetsPP.bShowMfgMotion = IsDlgButtonChecked(IDC_SHOW_MFG_MOTION) == BST_CHECKED;
	m_SetsPP.bShowDepthOverlay = IsDlgButtonChecked(IDC_SHOW_DEPTH_OVERLAY) == BST_CHECKED;
    // Retired A strength is kept only for Settings_t ABI / old preset compatibility.
    m_SetsPP.SetSvdLutStrength(0);
    for(int i=0;i<4;++i) {
        if(m_colorCaptured[i] && m_enhancementEnabled && !CommitNumericEdit(ColorLabels[i])) return E_FAIL;
    }
	m_SetsPP.bShowFeatureBadges = IsDlgButtonChecked(IDC_MFG_FEATURE_BADGES) == BST_CHECKED;
	m_SetsPP.bShowEditionDetails = IsDlgButtonChecked(IDC_EDITION_STATS_DETAILS) == BST_CHECKED;
    const LRESULT srLimit = ComboBox_GetCurItemData(m_extendedPane, IDC_DLSS_SR_SOURCE_LIMIT);
    if (srLimit < DLSSSR_SOURCE_Disable || srLimit > DLSSSR_SOURCE_1440p) return E_INVALIDARG;
    m_SetsPP.iDlssSRSourceLimit = static_cast<int>(srLimit);
    m_SetsPP.bDlssSR = srLimit != DLSSSR_SOURCE_Disable;
    m_SetsPP.bDlssSRDlaa = m_SetsPP.bDlssSR;
	const UINT mfgState = IsDlgButtonChecked(IDC_MFG_MULTIPLIER);
	if (mfgState != BST_CHECKED && mfgState != BST_UNCHECKED) return E_INVALIDARG;
	m_SetsPP.iMfgMultiplier = mfgState == BST_CHECKED ? 7u : 0u;
	const LRESULT processing = ComboBox_GetCurItemData(m_extendedPane, IDC_MFG_PROCESSING_MODE);
	if (processing != 0 && processing != 1) return E_INVALIDARG;
	m_SetsPP.iMfgProcessingMode = static_cast<unsigned>(processing);
	const LRESULT startup = ComboBox_GetCurItemData(m_extendedPane, IDC_EXTENDED_STARTUP);
	if (startup != 0 && startup != 1) return E_INVALIDARG;
	m_SetsPP.bWaitForExtendedStartup = startup == 1;
	return S_OK;
}

void CVRMainPPage::CommitPreviewSettings()
{
	m_oldSDRDisplayNits = m_SetsPP.iSDRDisplayNits;
    for(int i=0;i<4;++i) m_oldColorStrengths[i]=m_colorStrengths[i];
    m_oldEnhancementEnabled=m_enhancementEnabled;
	m_oldJvrFlatView = m_SetsPP.bJvrFlatView;
	m_oldJvrVertical = m_SetsPP.iJvrVertical;
	m_oldJvrReprojection = m_SetsPP.iJvrReprojection;
	m_oldJvrEye = m_SetsPP.iJvrEye;

}

bool CVRMainPPage::OnEditionMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // MPC-BE reactivates and invalidates the containing sheet on Apply.
    // The outer page clips its children, so ordinary ancestor invalidation
    // does not automatically invalidate the tab, footer or selected pane.
    // Propagate that paint to each visible child subtree, asynchronously;
    // never invalidate the outer page from its own paint or recreate panes.
    if (uMsg == WM_PAINT && m_hWnd && ::GetUpdateRect(m_hWnd, nullptr, FALSE)) {
        for (HWND child = ::GetWindow(m_hWnd, GW_CHILD); child;
            child = ::GetWindow(child, GW_HWNDNEXT)) {
            if (::IsWindowVisible(child)) {
                ::RedrawWindow(child, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            }
        }
        return false; // Let the native dialog procedure consume WM_PAINT.
    }
    if(uMsg==WM_NOTIFY && lParam) {
        const auto* header=reinterpret_cast<const NMHDR*>(lParam);
        if(header->idFrom==IDC_SETTINGS_TAB && header->code==TCN_SELCHANGE) {
            m_editionTab=::SendMessageW(GetDlgItem(IDC_SETTINGS_TAB),TCM_GETCURSEL,0,0)==1;
            ShowSettingsTab();
            return true;
        }
    }
	if (uMsg == WM_COMMAND) {
		LRESULT lValue;
		const int nID = LOWORD(wParam);
		const int action = HIWORD(wParam);
        bool numeric=nID==IDC_JVR_VERTICAL_VALUE || nID==IDC_JVR_REPROJECTION_VALUE;
        for(int id:ColorLabels) numeric |= nID==id;
        if(numeric && (action==EN_CHANGE || action==EN_KILLFOCUS)) {
            if(!m_updatingNumeric && m_bActivated) {
                if(action==EN_KILLFOCUS) CommitNumericEdit(nID);
                else SetDirty();
            }
            return true;
        }
		if (action == CBN_SELCHANGE && nID == IDC_JVR_EYE) {
			const LONG_PTR value = ComboBox_GetCurItemData(m_extendedPane, nID);
			if (value != CB_ERR) {
				m_SetsPP.iJvrEye = static_cast<int>(value);
				ApplyJvrPreview();
				SetDirty();
			}
			return TRUE;
		}

		if (action == BN_CLICKED) {
            if(nID==IDC_ENHANCEMENT_MASTER) {
                const bool requested=IsDlgButtonChecked(nID)==BST_CHECKED;
                if(PreviewEnhancementEnabled(requested)) SetDirty();
                else CheckDlgButton(nID,m_enhancementEnabled?BST_CHECKED:BST_UNCHECKED);
                EnableEditionControls();
                return true;
            }
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
				EnableEditionControls();
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
			if (nID == IDC_DEPTH_MODE) {
				const LRESULT mode = ComboBox_GetCurItemData(m_extendedPane, nID);
				if (mode >= ZIPDEPTH_Both && mode <= ZIPDEPTH_Disable) {
					edition::ApplyDepthMode(m_SetsPP, static_cast<int>(mode));
					EnableControls();
					SetDirty();
				}
				return (LRESULT)1;
			}
			if (nID == IDC_EXTENDED_STARTUP) {
				lValue = ComboBox_GetCurItemData(m_extendedPane, nID);
				if ((lValue == 0 || lValue == 1) && m_SetsPP.bWaitForExtendedStartup != (lValue == 1)) {
					m_SetsPP.bWaitForExtendedStartup = lValue == 1;
					SetDirty();
				}
				return (LRESULT)1;
			}
            if (nID == IDC_DLSS_SR_SOURCE_LIMIT) {
                const LRESULT choice = ComboBox_GetCurItemData(m_extendedPane, nID);
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
				lValue = ComboBox_GetCurItemData(m_extendedPane, nID);
				if ((lValue == 0 || lValue == 1) && m_SetsPP.iMfgProcessingMode != static_cast<unsigned>(lValue)) {
					m_SetsPP.iMfgProcessingMode = static_cast<unsigned>(lValue);
					SetDirty();
				}
				return (LRESULT)1;
			}
		}
	} else if (uMsg == WM_HSCROLL) {
        for(int i=0;i<4;++i) if((HWND)lParam==GetDlgItem(ColorIds[i])) {
            if(!m_enhancementEnabled) return true;
            const int value=int(SendDlgItemMessageW(ColorIds[i],TBM_GETPOS));
            UpdateColorStrengthLabel(i,value);
            if(LOWORD(wParam)!=TB_THUMBTRACK && value!=m_colorStrengths[i]) {
                if(PreviewColorStrength(i,value)) SetDirty();
                else {
                    SendDlgItemMessageW(ColorIds[i],TBM_SETPOS,TRUE,m_colorStrengths[i]);
                    UpdateColorStrengthLabel(i,m_colorStrengths[i]);
                }
            }
            return TRUE;
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

