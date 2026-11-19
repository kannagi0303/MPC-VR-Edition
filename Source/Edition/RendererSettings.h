// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atlbase.h>
#include <string>
#include "../IVideoRenderer.h"
namespace edition {
inline constexpr auto RendererRegistryKey = L"Software\\MPC-VR-MFG";
HRESULT VerifySavedSettings(CRegKey& key, const Settings_t& settings);
std::wstring PresetKeyPath(int slot);
}
