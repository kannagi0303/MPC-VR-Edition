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
	int m_oldSvdLutStrength = 0;
    int m_svdLutBStrength = 200;
    int m_oldSvdLutBStrength = 200;
    bool m_svdLutBPreviewed = false;
	bool m_svdLutPreviewed = false;
	bool m_svdLutOff = false;
	bool m_oldSvdLutOff = false;
	bool m_svdLutRaw = false;
	bool m_oldSvdLutRaw = false;
	bool m_svdLutTemporal = false;
	bool m_oldSvdLutTemporal = false;
    bool m_svdLutB2Luminance = false;
    bool m_oldSvdLutB2Luminance = false;
    bool m_svdLutB2Memory = false;
    bool m_oldSvdLutB2Memory = false;
    bool m_svdLutR1Gamut = false;
    bool m_oldSvdLutR1Gamut = false;
    bool m_skinTones = false;
    bool m_oldSkinTones = false;
    bool m_preferredColors = false;
    bool m_oldPreferredColors = false;

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
	void UpdateSvdLutLabel();
	void PreviewSvdLutStrength(int strength);
    void PreviewSvdLutBStrength(int strength);
	void PreviewSvdLutOff(bool off);
	void PreviewSvdLutRaw(bool raw);
	void PreviewSvdLutTemporal(bool temporal);
    void PreviewSvdLutB2Luminance(bool enable);
    void PreviewSvdLutB2Memory(bool enable);
    void PreviewSvdLutR1Gamut(bool enable);
    void PreviewSkinTones(bool enable);
    void PreviewPreferredColors(bool enable);

	HRESULT OnConnect(IUnknown* pUnknown) override;
	HRESULT OnDisconnect() override;
	HRESULT OnActivate() override;
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
