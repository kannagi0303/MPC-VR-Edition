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
#include "Edition/SettingsRegistry.h"
#include "Edition/RendererSettings.h"

namespace edition {
// Legacy migration remains unused; this refactor must not enable it.
#define OPT_REGKEY_VIDEORENDERER RendererRegistryKey
#define OPT_REGKEY_ORIGINAL L"Software\\MPC-BE Filters\\MPC Video Renderer"
// The settings are this renderer's own. The original is often on the same machine -- MPC-HC
// carries it in its MPCVR folder, MPC-BE installs it -- and a key shared with it was read
// by a renderer that knows none of the methods added to the lists here, which it set back
// to its defaults and wrote back so the first time its page was applied. The first time,
// the original's key is copied, settings and presets, and left as it is; a key of ours
// that exists, even empty, is never copied over (Reset_Settings.cmd leaves one).
static void TakeSettingsOver()
{
	CRegKey plus;
	if (ERROR_SUCCESS == plus.Open(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER, KEY_READ)) {
		return;
	}
	CRegKey original;
	if (ERROR_SUCCESS != original.Open(HKEY_CURRENT_USER, OPT_REGKEY_ORIGINAL, KEY_READ)) {
		return;
	}
	// RegCopyTreeW wants every right on the key it copies into: with KEY_READ | KEY_WRITE
	// it is ERROR_ACCESS_DENIED and nothing is copied. A copy that fails takes its empty
	// key away again, or the defaults would stay for good; the next start tries again.
	if (ERROR_SUCCESS == plus.Create(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER, REG_NONE, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS)) {
		const LSTATUS ret = RegCopyTreeW(original, nullptr, plus);
		if (ret != ERROR_SUCCESS) {
			DLog(L"TakeSettingsOver() : the original's settings were not copied, error {}", ret);
			plus.Close();
			RegDeleteTreeW(HKEY_CURRENT_USER, OPT_REGKEY_VIDEORENDERER);
		}
	}
}

// Presets: ten keys beside the settings, each holding a whole Settings_t under
// the same names, the name the user gave it and the key that recalls it.
std::wstring PresetKeyPath(int slot)
{
	return std::format(L"{}\\Presets\\{}", OPT_REGKEY_VIDEORENDERER, slot + 1);
}


HRESULT VerifySavedSettings(CRegKey& key, const Settings_t& sets)
{
	DWORD saved = 0;
	const LSTATUS read = key.QueryDWORDValue(L"MfgMultiplier", saved);
	if (read != ERROR_SUCCESS) return HRESULT_FROM_WIN32(read);
	DWORD savedMode = 0;
	if (key.QueryDWORDValue(L"MfgProcessingMode", savedMode) != ERROR_SUCCESS) return E_FAIL;
	DWORD savedStartup = 0;
	if (key.QueryDWORDValue(L"WaitForExtendedStartup", savedStartup) != ERROR_SUCCESS) return E_FAIL;
	DWORD savedDepthMode = 0;
	if (key.QueryDWORDValue(ResearchDepthModeKey, savedDepthMode) != ERROR_SUCCESS) return E_FAIL;
	DWORD savedSvdLut = 0;
	DWORD savedSvdLutStrength = 0;
	if (key.QueryDWORDValue(L"SvdLut", savedSvdLut) != ERROR_SUCCESS ||
		key.QueryDWORDValue(L"SvdLutStrength", savedSvdLutStrength) != ERROR_SUCCESS) return E_FAIL;
	DWORD savedDepthOverlay = 0;
	if (key.QueryDWORDValue(L"ShowDepthOverlay", savedDepthOverlay) != ERROR_SUCCESS) return E_FAIL;
	DWORD savedLowMotion = 1, savedDepth3D = 1;
	if (key.QueryDWORDValue(L"MfgLowMotion", savedLowMotion) != ERROR_SUCCESS ||
		key.QueryDWORDValue(L"Depth3DPreview", savedDepth3D) != ERROR_SUCCESS) return E_FAIL;
	return saved == sets.iMfgMultiplier && savedMode == sets.iMfgProcessingMode &&
		savedStartup == static_cast<DWORD>(sets.bWaitForExtendedStartup) &&
		savedDepthMode == static_cast<DWORD>(edition::DepthModeFromSettings(sets)) &&
		savedDepthOverlay == static_cast<DWORD>(sets.bShowDepthOverlay) &&
		savedSvdLutStrength == static_cast<DWORD>(sets.GetSvdLutStrength()) &&
		savedSvdLut == static_cast<DWORD>(sets.GetSvdLutStrength() != 0) &&
		savedLowMotion == 0 && savedDepth3D == 0 ? S_OK : E_FAIL;
}
}
