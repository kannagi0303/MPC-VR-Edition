// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atlbase.h>
#include "../IVideoRenderer.h"

// Edition settings only. Global settings and presets share persistence.
namespace edition {
inline constexpr wchar_t ResearchDepthModeKey[] = L"ResearchDepthMode";

inline int DepthModeFromSettings(const Settings_t& settings)
{
	if (settings.bUseZipDepthSr) return settings.bUseZipDepthMfg ? ZIPDEPTH_Both : ZIPDEPTH_DlssSrOnly;
	return settings.bUseZipDepthMfg ? ZIPDEPTH_MfgOnly : ZIPDEPTH_Disable;
}

inline void ApplyDepthMode(Settings_t& settings, const int mode)
{
	settings.bUseZipDepthSr = mode == ZIPDEPTH_Both || mode == ZIPDEPTH_DlssSrOnly;
	settings.bUseZipDepthMfg = mode == ZIPDEPTH_Both || mode == ZIPDEPTH_MfgOnly;
}

void ReadSettings(CRegKey& key, Settings_t& settings);
void WriteSettings(CRegKey& key, const Settings_t& settings);
}
