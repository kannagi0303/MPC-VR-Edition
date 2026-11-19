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
#include "Edition/RendererSettings.h"
#include "Edition/SettingsRegistry.h"
#include "Mfg/SettingsScope.h"

// The keyboard hook is a plain callback, so it needs a way back to the filter.
// One renderer per process in practice; if a second one appears the last to
// install wins, which is no worse than two filters fighting over one key.
static CMpcVideoRenderer* g_pToggleKeyOwner = nullptr;

// ---------------------------------------------------------------------------
// DLSS toggle key
//
// A thread-local WH_KEYBOARD hook on the thread that owns the video window, so
// it sees only that thread's input -- no system-wide hook. It swallows the one
// key it is bound to and passes everything else through untouched, which
// matters because the default (Home) is also a seek shortcut in most players.
// ---------------------------------------------------------------------------

LRESULT CALLBACK CMpcVideoRenderer::ToggleKeyProc(int code, WPARAM wParam, LPARAM lParam)
{
	CMpcVideoRenderer* pThis = g_pToggleKeyOwner;

	if (code == HC_ACTION && pThis && wParam) {
		// What is held with the key is only asked once a preset wants that key, and a
		// caret means somebody is typing: a Shift+1 meant for a file name is not ours.
		int held = -1;
		for (int slot = 0; slot < PRESET_COUNT; slot++) {
			if (!pThis->m_presetKeys[slot] || (int)wParam != (pThis->m_presetKeys[slot] & 0xFF)) {
				continue;
			}
			if (held < 0) {
				GUITHREADINFO gui = { sizeof(gui) };
				if (GetGUIThreadInfo(GetCurrentThreadId(), &gui) && gui.hwndCaret) {
					break;
				}
				held = (GetKeyState(VK_SHIFT) < 0 ? PRESETKEY_Shift : 0)
				     | (GetKeyState(VK_CONTROL) < 0 ? PRESETKEY_Ctrl : 0)
				     | (GetKeyState(VK_MENU) < 0 ? PRESETKEY_Alt : 0);
			}
			if (((int)wParam | held) == pThis->m_presetKeys[slot]) {
				const bool bKeyUp = (lParam & 0x80000000) != 0;
				const bool bRepeat = (lParam & 0x40000000) != 0;
				if (!bKeyUp && !bRepeat) {
					pThis->RecallPreset(slot);
				}
				return 1; // consumed, like the toggle key
			}
		}
	}

	if (code == HC_ACTION && pThis && pThis->m_Sets.iDlssNRToggleKey
			&& (int)wParam == pThis->m_Sets.iDlssNRToggleKey) {
		const bool bKeyUp = (lParam & 0x80000000) != 0;
		const bool bRepeat = (lParam & 0x40000000) != 0;

		if (!bKeyUp && !bRepeat) {
			pThis->Flt_SetBool("dlssNR", !pThis->m_Sets.bDlssNR);
		}
		return 1; // consumed, so the player does not also act on it
	}

	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void CMpcVideoRenderer::ReadPresetKeys()
{
	for (int slot = 0; slot < PRESET_COUNT; slot++) {
		m_presetKeys[slot] = 0;
		GetPresetInfo(slot, nullptr, 0, &m_presetKeys[slot]);
	}
}

// A preset recalled by its key: applied as the page's Apply would, and kept as the
// settings in force so the next film starts with it.
void CMpcVideoRenderer::RecallPreset(int slot)
{
	Settings_t sets;
	if (S_OK != LoadPreset(slot, sets)) {
		return;
	}
	KeepOutOfPreset(sets, m_Sets);   // the statistics among them, up or not as they are

	// The name goes up before the settings go in. A film that is paused is drawn again
	// by the settings themselves and by nothing after: with the name posted afterwards
	// that picture went out without it, and the next recall showed the name of this
	// one -- or none, or the wrong one, as the redraws fell.
	wchar_t name[64] = {};
	GetPresetInfo(slot, name, (int)std::size(name), nullptr);
	m_VideoProcessor->ShowNotice(name[0] ? std::format(L"Preset {}: {}", slot + 1, name)
	                                     : std::format(L"Preset {}", slot + 1));
	SetSettings(sets);
	SaveSettings();

	// Paused, nothing draws the name unless asked, and nothing takes it down either.
	if (m_filterState != State_Running) {
		Redraw();
	}
	if (m_hNoticeTimer) {
		DeleteTimerQueueTimer(nullptr, m_hNoticeTimer, INVALID_HANDLE_VALUE);
		m_hNoticeTimer = nullptr;
	}
	CreateTimerQueueTimer(&m_hNoticeTimer, nullptr, NoticeTimerProc, this, 2600, 0, WT_EXECUTEONLYONCE);
}

void CALLBACK CMpcVideoRenderer::NoticeTimerProc(void* pParam, BOOLEAN)
{
	auto pThis = static_cast<CMpcVideoRenderer*>(pParam);
	if (pThis->m_filterState != State_Running) {
		pThis->Redraw();
	}
}

STDMETHODIMP CMpcVideoRenderer::SavePreset(int slot, const Settings_t& sets, LPCWSTR name, int vk)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	CRegKey key;
	if (ERROR_SUCCESS != key.Create(HKEY_CURRENT_USER, edition::PresetKeyPath(slot).c_str())) {
		return E_FAIL;
	}
	WriteSettings(key, sets);
	key.SetDWORDValue(L"Saved", 1);
	key.Close();
	return SetPresetInfo(slot, name, vk);
}

STDMETHODIMP CMpcVideoRenderer::LoadPreset(int slot, Settings_t& sets)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	CRegKey key;
	DWORD saved = 0;
	if (ERROR_SUCCESS != key.Open(HKEY_CURRENT_USER, edition::PresetKeyPath(slot).c_str(), KEY_READ)
			|| ERROR_SUCCESS != key.QueryDWORDValue(L"Saved", saved) || !saved) {
		return S_FALSE;   // nothing was ever saved here
	}
	sets.SetDefault();
	ReadSettings(key, sets);
	return S_OK;
}

STDMETHODIMP CMpcVideoRenderer::GetPresetInfo(int slot, LPWSTR name, int cchName, int* pVk)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	if (name && cchName > 0) {
		name[0] = 0;
	}
	if (pVk) {
		*pVk = 0;
	}
	CRegKey key;
	if (ERROR_SUCCESS != key.Open(HKEY_CURRENT_USER, edition::PresetKeyPath(slot).c_str(), KEY_READ)) {
		return S_FALSE;
	}
	if (name && cchName > 0) {
		ULONG chars = (ULONG)cchName;
		if (ERROR_SUCCESS != key.QueryStringValue(L"Name", name, &chars)) {
			name[0] = 0;
		}
	}
	DWORD dw = 0;
	if (pVk && ERROR_SUCCESS == key.QueryDWORDValue(L"Key", dw)) {
		*pVk = (int)(dw & PRESETKEY_Mask);
	}
	DWORD saved = 0;
	return (ERROR_SUCCESS == key.QueryDWORDValue(L"Saved", saved) && saved) ? S_OK : S_FALSE;
}

STDMETHODIMP CMpcVideoRenderer::SetPresetInfo(int slot, LPCWSTR name, int vk)
{
	if (slot < 0 || slot >= PRESET_COUNT) {
		return E_INVALIDARG;
	}
	CRegKey key;
	if (ERROR_SUCCESS != key.Create(HKEY_CURRENT_USER, edition::PresetKeyPath(slot).c_str())) {
		return E_FAIL;
	}
	key.SetStringValue(L"Name", name ? name : L"");
	key.SetDWORDValue(L"Key", (DWORD)(vk & PRESETKEY_Mask));
	key.Close();
	ReadPresetKeys();
	InstallToggleKeyHook();
	return S_OK;
}

void CMpcVideoRenderer::InstallToggleKeyHook()
{
	bool bAnyKey = m_Sets.iDlssNRToggleKey != 0;
	for (const int vk : m_presetKeys) {
		bAnyKey = bAnyKey || vk != 0;
	}
	if (!bAnyKey) {
		RemoveToggleKeyHook();
		return;
	}

	// Keys arrive on the player's UI thread, which owns its main window -- not
	// necessarily the thread that called Init(). Hooking our own thread saw no
	// keystrokes at all, which is why the shortcut did nothing.
	HWND hTarget = m_hWndParentMain ? m_hWndParentMain
	             : (m_hWndParent ? m_hWndParent : m_hWndWindow);
	if (!hTarget) {
		return;
	}
	const DWORD dwThread = GetWindowThreadProcessId(hTarget, nullptr);
	if (!dwThread) {
		return;
	}
	if (m_hKeyboardHook && dwThread == m_dwHookedThread) {
		return;
	}

	RemoveToggleKeyHook();
	g_pToggleKeyOwner = this;
	m_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD, &CMpcVideoRenderer::ToggleKeyProc,
	                                    nullptr, dwThread);
	m_dwHookedThread = m_hKeyboardHook ? dwThread : 0;
	DLogIf(!m_hKeyboardHook, L"CMpcVideoRenderer::InstallToggleKeyHook() : failed on thread {}, error {}",
		dwThread, GetLastError());
}

void CMpcVideoRenderer::RemoveToggleKeyHook()
{
	if (m_hKeyboardHook) {
		UnhookWindowsHookEx(m_hKeyboardHook);
		m_hKeyboardHook = nullptr;
	}
	m_dwHookedThread = 0;
	if (g_pToggleKeyOwner == this) {
		g_pToggleKeyOwner = nullptr;
	}
}

