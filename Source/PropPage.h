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

#pragma once

#include "IVideoRenderer.h"
#include <string>
#include <vector>

// Shared controls for the native page and its Edition section.
void ComboBox_AddStringData(HWND hWnd, int nIDComboBox, LPCWSTR str, LONG_PTR data);
LONG_PTR ComboBox_GetCurItemData(HWND hWnd, int nIDComboBox);
void ComboBox_SelectByItemData(HWND hWnd, int nIDComboBox, LONG_PTR data);

// CVRMainPPage

class __declspec(uuid("DA46D181-07D6-441D-B314-019AEB10148A"))
	CVRMainPPage : public CBasePropertyPage, public CWindow
{
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

	Settings_t m_SetsPP;

	int m_oldSDRDisplayNits = SDR_NITS_DEF;
    int m_colorStrengths[5]{100,100,100,100,1};
    int m_oldColorStrengths[5]{100,100,100,100,1};
    bool m_colorCaptured[5]{};
    bool m_editionTab=false;
    bool m_enhancementEnabled=true, m_oldEnhancementEnabled=true, m_masterCaptured=false;
    bool m_updatingNumeric=false;
    bool PreviewEnhancementEnabled(bool enabled);
    bool CommitNumericEdit(int id);
    void SetNumericEdit(int id,int value);
    HWND m_rendererPane = nullptr, m_extendedPane = nullptr;
    HRESULT InitializeSettingsTabs();
    static INT_PTR CALLBACK SettingsPaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // Keep original and Edition control call sites independent of their parent.
    HWND ControlParent(int id) const;
    CWindow GetDlgItem(int id) const { return CWindow(::GetDlgItem(ControlParent(id), id)); }
    LRESULT SendDlgItemMessageW(int id, UINT msg, WPARAM wp = 0, LPARAM lp = 0) const {
        return ::SendDlgItemMessageW(ControlParent(id), id, msg, wp, lp);
    }
    BOOL SetDlgItemTextW(int id, LPCWSTR text) const { return ::SetDlgItemTextW(ControlParent(id), id, text); }
    UINT GetDlgItemInt(int id, BOOL* translated, BOOL isSigned) const { return ::GetDlgItemInt(ControlParent(id), id, translated, isSigned); }
    BOOL CheckDlgButton(int id, UINT state) const { return ::CheckDlgButton(ControlParent(id), id, state); }
    UINT IsDlgButtonChecked(int id) const { return ::IsDlgButtonChecked(ControlParent(id), id); }
    void ShowSettingsTab();
    void ResetColorStrengths();

	bool m_oldJvrFlatView = false;
	int m_oldJvrVertical = JVR_VERTICAL_DEFAULT;
	int m_oldJvrReprojection = JVR_REPROJECTION_DEFAULT;
	int m_oldJvrEye = 0;
	bool m_showJvrOptions = false;

	bool m_bActivated = false;
	HWND m_hHint = nullptr;

public:
	CVRMainPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRMainPPage();

private:
	// Edition page lifecycle. Preview rollback is one transaction with SDR nits.
	void SetEditionControls();
	void EnableEditionControls();
	void CapturePreviewSettings();
	void RestorePreviewSettings();
	void CommitPreviewSettings();
	HRESULT InitializeEditionControls();
	void AddEditionHints();
	HRESULT ReadEditionControls();
	bool OnEditionMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

	void SetControls();
	void EnableControls();
	void UpdateJvrViewLabels();
	void ApplyJvrPreview();
    bool PreviewColorStrength(int index,int value);
    void UpdateColorStrengthLabel(int index,int value);

	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
    HRESULT OnDeactivate() override;
	void SetDirty() {
		if (m_bActivated && !m_bDirty) {
			m_bDirty = TRUE;
			if (m_pPageSite) {
				m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
			}
		}
	}
	INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override;
	HRESULT OnApplyChanges() override;

	HWND CreateHintWindow(HWND parent, int timePop = 1700, int timeInit = 70, int timeReshow = 7);
	void AddHint(int id, const LPCWSTR text);
};

// CVRInfoPPage

class __declspec(uuid("D697132B-FCA4-4401-8869-D3B39D0750DB"))
	CVRInfoPPage : public CBasePropertyPage, public CWindow
{
	HFONT m_hMonoFont = nullptr;
	CComQIPtr<IVideoRenderer> m_pVideoRenderer;

public:
	CVRInfoPPage(LPUNKNOWN lpunk, HRESULT* phr);
	~CVRInfoPPage();

private:
	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
};
