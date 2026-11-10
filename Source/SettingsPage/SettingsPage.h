// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../IVideoRenderer.h"

class __declspec(uuid("86A87F09-5AA7-46A7-A163-A94D006D3BF3")) CVRSettingsPPage : public CBasePropertyPage {
    CComQIPtr<IVideoRenderer> renderer_;
    bool active_ = false;
    unsigned initialMultiplier_ = 0;
    HRESULT lastSaveResult_ = S_OK;

    bool SelectedMultiplier(unsigned& value) const;
    void Fill();
    void Status();
    void MarkDirty();

    HRESULT OnConnect(IUnknown*) override;
    HRESULT OnDisconnect() override;
    HRESULT OnActivate() override;
    HRESULT OnDeactivate() override;
    HRESULT OnApplyChanges() override;
    INT_PTR OnReceiveMessage(HWND, UINT, WPARAM, LPARAM) override;
public:
    CVRSettingsPPage(LPUNKNOWN, HRESULT*);
};
