// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "SettingsPage.h"
#include "resource.h"

namespace {
constexpr UINT WM_MFG_COMMIT = WM_APP + 0x31A;

unsigned NormalizeMultiplier(unsigned value) {
    return value >= 2 && value <= 7 ? 7u : 0u;
}
std::wstring MfgLabel(unsigned value) {
    value = NormalizeMultiplier(value);
    return value ? L"Auto" : L"Off";
}
}

CVRSettingsPPage::CVRSettingsPPage(LPUNKNOWN unknown, HRESULT* result)
    : CBasePropertyPage(L"MFG", unknown, IDD_MFGPAGE, IDS_SETTINGSPAGE_TITLE) {
    if (result) *result = S_OK;
}

// Use the interface that was proven to work with MPC-BE's property-page host.
// No extra QueryInterface request is needed during property page creation.
HRESULT CVRSettingsPPage::OnConnect(IUnknown* unknown) {
    if (!unknown) return E_POINTER;
    renderer_ = unknown;
    return renderer_ ? S_OK : E_NOINTERFACE;
}

HRESULT CVRSettingsPPage::OnDisconnect() {
    renderer_.Release();
    return S_OK;
}

bool CVRSettingsPPage::SelectedMultiplier(unsigned& value) const {
    if (!m_hwnd) return false;
    const UINT checked = IsDlgButtonChecked(m_hwnd, IDC_MFG_MULTIPLIER);
    if (checked != BST_CHECKED && checked != BST_UNCHECKED) return false;
    value = checked == BST_CHECKED ? 7u : 0u;
    return true;
}

void CVRSettingsPPage::Fill() {
    Settings_t current;
    renderer_->GetSettings(current);
    initialMultiplier_ = NormalizeMultiplier(current.iMfgMultiplier);
    CheckDlgButton(m_hwnd, IDC_MFG_MULTIPLIER, initialMultiplier_ ? BST_CHECKED : BST_UNCHECKED);
}

void CVRSettingsPPage::MarkDirty() {
    if (!active_) return;
    m_bDirty = TRUE;
    if (m_pPageSite) m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
}

void CVRSettingsPPage::Status() {
    if (!renderer_ || !m_hwnd) return;
    unsigned ui = 0;
    Settings_t current;
    renderer_->GetSettings(current);

    std::wstring status = L"UI: ";
    status += SelectedMultiplier(ui) ? MfgLabel(ui) : L"Unavailable";
    status += L"\r\nRenderer: " + MfgLabel(current.iMfgMultiplier);

    CRegKey key;
    DWORD saved = 0;
    const bool found = key.Open(HKEY_CURRENT_USER, L"Software\\MPC-VR-MFG", KEY_READ) == ERROR_SUCCESS &&
        key.QueryDWORDValue(L"MfgMultiplier", saved) == ERROR_SUCCESS;
    status += L"\r\nSaved: ";
    status += found ? MfgLabel(saved) : L"not present";
    if (FAILED(lastSaveResult_)) {
        status += std::format(L"\r\nSave error: 0x{:08X}", static_cast<unsigned>(lastSaveResult_));
    }
    if (renderer_->GetActive()) {
        std::wstring runtime;
        if (SUCCEEDED(renderer_->GetVideoProcessorInfo(runtime))) {
            status += L"\r\n--- Runtime ---\r\n";
            for (const wchar_t c : runtime) {
                if (c == L'\n' && (status.empty() || status.back() != L'\r')) status += L'\r';
                status += c;
            }
        }
    } else {
        status += L"\r\n--- Runtime ---\r\nNo connected input (MFG not started)";
    }
    SetDlgItemTextW(m_hwnd, IDC_MFG_STATUS, status.c_str());
    if (const HWND edit = GetDlgItem(m_hwnd, IDC_MFG_STATUS)) {
        SendMessageW(edit, EM_SETSEL, 0, 0);
        SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    }
}

HRESULT CVRSettingsPPage::OnActivate() {
    active_ = false;
    lastSaveResult_ = S_OK;
    Fill();
    Status();
    SetTimer(m_hwnd, 1, 1000, nullptr);
    active_ = true;
    return S_OK;
}

HRESULT CVRSettingsPPage::OnDeactivate() {
    // A host may close a property page without dispatching a dirty notification.
    // Read the control while the HWND still exists; this never depends on NGX.
    unsigned selected = 0;
    if (active_ && renderer_ && SelectedMultiplier(selected) && selected != initialMultiplier_) {
        OnApplyChanges();
    }
    active_ = false;
    KillTimer(m_hwnd, 1);
    return S_OK;
}

HRESULT CVRSettingsPPage::OnApplyChanges() {
    if (!renderer_) return E_UNEXPECTED;
    unsigned selected = 0;
    if (!SelectedMultiplier(selected)) return E_INVALIDARG;

    // Merge only this field into the current state. The native Main page
    // continues to own all original MPC-VR options.
    Settings_t current;
    renderer_->GetSettings(current);
    if (current.iMfgMultiplier != selected) {
        current.iMfgMultiplier = selected;
        renderer_->SetSettings(current);
    }
    Settings_t accepted;
    renderer_->GetSettings(accepted);
    if (accepted.iMfgMultiplier != selected) {
        lastSaveResult_ = E_FAIL;
    } else {
        lastSaveResult_ = renderer_->SaveSettings();
        if (SUCCEEDED(lastSaveResult_)) {
            CRegKey key;
            DWORD saved = 0;
            const LSTATUS opened = key.Open(HKEY_CURRENT_USER, L"Software\\MPC-VR-MFG", KEY_READ);
            const LSTATUS read = opened == ERROR_SUCCESS ? key.QueryDWORDValue(L"MfgMultiplier", saved) : opened;
            lastSaveResult_ = read != ERROR_SUCCESS ? HRESULT_FROM_WIN32(read)
                : saved != selected ? E_FAIL : S_OK;
        }
    }
    if (SUCCEEDED(lastSaveResult_)) {
        initialMultiplier_ = selected;
        m_bDirty = FALSE;
    } else {
        MarkDirty();
    }
    Status();
    return lastSaveResult_;
}

INT_PTR CVRSettingsPPage::OnReceiveMessage(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER && wp == 1 && active_) {
        Status();
        return TRUE;
    }
    if (message == WM_MFG_COMMIT && active_) {
        // Commit outside the ComboBox notification callback to avoid reentrancy
        // into Renderer::Configure while the drop-down is processing its state.
        OnApplyChanges();
        return TRUE;
    }
    if (message == WM_COMMAND && active_ && LOWORD(wp) == IDC_MFG_MULTIPLIER) {
        const WORD notice = HIWORD(wp);
        if (notice == BN_CLICKED) {
            MarkDirty();
            PostMessageW(m_hwnd, WM_MFG_COMMIT, 0, 0);
            return TRUE;
        }
    }
    return CBasePropertyPage::OnReceiveMessage(hwnd, message, wp, lp);
}
