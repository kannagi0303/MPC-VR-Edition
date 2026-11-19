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
#include "Mfg/SettingsScope.h"

#define OPT_DlssNR                         L"DlssNeuralRendering"
#define OPT_DlssNRStyle                    L"DlssNRStyle"
#define OPT_DlssNRPreset                   L"DlssNRPreset"
#define OPT_DlssNRIntensity                L"DlssNRIntensity"
#define OPT_DlssNRLocalTone                L"DlssNRLocalToneStrength"
#define OPT_DlssNRLocalStructure           L"DlssNRLocalStructureStrength"
#define OPT_DlssNRSkinStructure            L"DlssNRSkinStructureStrength"
#define OPT_DlssNRAutoMask                 L"DlssNRUseAutoMask"
#define OPT_DlssNRAfterUpscale             L"DlssNRApplyAfterUpscaling"
#define OPT_DlssNRToggleKey                L"DlssNRToggleKey"
#define OPT_DlssNRDllPath                  L"DlssNRDllPath"
#define OPT_DlssNRStabilizer               L"DlssNRStabilizerStrength"
#define OPT_DlssNRWorkingScale             L"DlssNRWorkingScale"
#define OPT_DlssNRFlowDivider              L"DlssNRFlowDivider"
#define OPT_DlssSR                         L"DlssSRUpscaling"
#define OPT_DlssSRSourceLimit              L"DlssSRSourceLimit"
#define OPT_DlssSRDllPath                  L"DlssSRDllPath"
#define OPT_DlssSRDlaa                     L"DlssSRDlaa"
#define OPT_UpscalingSmall                 L"UpscalingSmall"
#define OPT_UpscalingSmallFilter           L"UpscalingSmallFilter"
#define OPT_DlssRenderAhead                L"DlssRenderAhead"
#define OPT_VideoDelay                     L"VideoDelay"
#define OPT_VPSuperResolution              L"VPSuperResolution"
#define OPT_ChromaUpsampling               L"ChromaUpsampling"
#define OPT_Upscaling                      L"Upscaling"

#define OPT_VPReplaceChroma                L"VPReplaceChroma"
#define OPT_Sharpen                        L"Sharpen"
#define OPT_SharpenLevel                   L"SharpenLevel"
#define OPT_Deblock                        L"Deblock"
#define OPT_DeblockLevel                   L"DeblockLevel"
#define OPT_Deband                         L"Deband"
#define OPT_DebandLevel                    L"DebandLevel"
#define OPT_HideLogo                       L"HideLogo"
#define OPT_LogoMargin                     L"LogoMargin"

namespace edition {
void ReadSettings(CRegKey& key, Settings_t& sets)
{
	DWORD dw;
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPReplaceChroma, dw)) {
		sets.bVPReplaceChroma = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Sharpen, dw)) {
		sets.iSharpen = discard<int>(dw, SHARPEN_Disabled, 0, SHARPEN_COUNT - 1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_SharpenLevel, dw)) {
		sets.iSharpenLevel = discard<int>(dw, SHARPEN_LEVEL_DEF, SHARPEN_LEVEL_MIN, SHARPEN_LEVEL_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Deblock, dw)) {
		sets.iDeblock = discard<int>(dw, DEBLOCK_Off, DEBLOCK_Off, DEBLOCK_COUNT - 1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DeblockLevel, dw)) {
		sets.iDeblockLevel = discard<int>(dw, CLEAN_LEVEL_DEF, CLEAN_LEVEL_MIN, CLEAN_LEVEL_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Deband, dw)) {
		sets.bDeband = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DebandLevel, dw)) {
		sets.iDebandLevel = discard<int>(dw, CLEAN_LEVEL_DEF, CLEAN_LEVEL_MIN, CLEAN_LEVEL_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_HideLogo, dw)) {
		sets.bHideLogo = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_LogoMargin, dw)) {
		sets.iLogoMargin = discard<int>(dw, LOGO_MARGIN_DEF, LOGO_MARGIN_MIN, LOGO_MARGIN_MAX);
	}
	// Override only the Edition-specific legacy range and invalid-value defaults
	// after the native renderer has read its own fields. Do not duplicate its table.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VPSuperResolution, dw)) {
		// Old Always (5) now maps to the highest supported threshold.
		sets.iVPSuperRes = dw == SUPERRES_Always ? SUPERRES_1440p
			: discard<int>(dw, SUPERRES_Disable, 0, SUPERRES_DLSS_SR);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_ChromaUpsampling, dw)) {
		sets.iChromaScaling = discard<int>(dw, CHROMA_CatmullRom, 0, CHROMA_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_Upscaling, dw)) {
		sets.iUpscaling = discard<int>(dw, UPSCALE_Jinc2, 0, UPSCALE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(L"MfgMultiplier", dw)) {
		// Migrate legacy fixed x2-x6 to the sole supported Auto mode.
		sets.iMfgMultiplier = dw >= 2 && dw <= 7 ? 7u : 0u;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(L"MfgProcessingMode", dw)) {
		sets.iMfgProcessingMode = dw <= 1 ? dw : 0;
	}
	sets.bWaitForExtendedStartup = key.QueryDWORDValue(L"WaitForExtendedStartup", dw) == ERROR_SUCCESS && dw == 1;
	if (ERROR_SUCCESS == key.QueryDWORDValue(L"ShowFeatureBadges", dw)) {
		sets.bShowFeatureBadges = dw != 0;
	}
#ifdef _WIN64
	// The DLSS 5 NR snippet is x64 only.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNR, dw)) {
		sets.bDlssNR = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRStyle, dw)) {
		sets.iDlssNRStyle = discard<int>((int)dw, DLSSNR_STYLE_Default, 0, DLSSNR_STYLE_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRPreset, dw)) {
		sets.iDlssNRPreset = discard<int>((int)dw, 0, 0, DLSSNR_PRESET_COUNT-1);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRIntensity, dw)) {
		sets.iDlssNRIntensity = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_STR_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRLocalTone, dw)) {
		sets.iDlssNRLocalTone = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_STR_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRLocalStructure, dw)) {
		sets.iDlssNRLocalStructure = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_STR_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRSkinStructure, dw)) {
		// Negative values round-trip through DWORD as two's complement.
		sets.iDlssNRSkinStructure = discard<int>((int)dw, DLSSNR_STR_DEF, DLSSNR_SKIN_MIN, DLSSNR_STR_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRAutoMask, dw)) {
		sets.bDlssNRAutoMask = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRAfterUpscale, dw)) {
		sets.bDlssNRAfterUpscale = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRStabilizer, dw)) {
		sets.iDlssNRStabilizer = discard<int>((int)dw, DLSSNR_STAB_DEF, DLSSNR_STAB_MIN, DLSSNR_STAB_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRWorkingScale, dw)) {
		sets.iDlssNRWorkingScale = discard<int>((int)dw, DLSSNR_SCALE_DEF, DLSSNR_SCALE_MIN, DLSSNR_SCALE_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRFlowDivider, dw)) {
		sets.iDlssNRFlowDivider = discard<int>((int)dw, DLSSNR_FLOW_DEF, DLSSNR_FLOW_AUTO, DLSSNR_FLOW_MAX);
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssNRToggleKey, dw)) {
		sets.iDlssNRToggleKey = discard<int>((int)dw, VK_F12, 0, 0xFF);
	}
	{
		ULONG nChars = std::size(sets.szDlssNRDllPath);
		if (ERROR_SUCCESS != key.QueryStringValue(OPT_DlssNRDllPath, sets.szDlssNRDllPath, &nChars)) {
			sets.szDlssNRDllPath[0] = L'\0';
		}
		sets.szDlssNRDllPath[std::size(sets.szDlssNRDllPath) - 1] = L'\0';
	}
	// DLSS Super Resolution, x64 as well: the NGX runtime has no 32-bit build.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssSR, dw)) {
		sets.bDlssSR = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssSRDlaa, dw)) {
		sets.bDlssSRDlaa = !!dw;
	}
    if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssSRSourceLimit, dw)) {
        sets.iDlssSRSourceLimit = dw <= DLSSSR_SOURCE_1440p ? static_cast<int>(dw) : DLSSSR_SOURCE_Disable;
        sets.bDlssSR = sets.iDlssSRSourceLimit != DLSSSR_SOURCE_Disable;
    } else {
        // Upgrade older checkbox-based configurations without losing SR.
        sets.iDlssSRSourceLimit = sets.bDlssSR ? DLSSSR_SOURCE_1440p : DLSSSR_SOURCE_Disable;
    }
	mfg::NormalizeDlssSR(sets);
	{
		ULONG nChars = std::size(sets.szDlssSRDllPath);
		if (ERROR_SUCCESS != key.QueryStringValue(OPT_DlssSRDllPath, sets.szDlssSRDllPath, &nChars)) {
			sets.szDlssSRDllPath[0] = L'\0';
		}
		sets.szDlssSRDllPath[std::size(sets.szDlssSRDllPath) - 1] = L'\0';
	}
#endif
	// New single ZipDepth routing mode. If absent, preserve the last checkbox-based
	// configuration from older builds; completely new installs keep the default Both.
	DWORD zipDepthMode = ZIPDEPTH_Both;
	if (key.QueryDWORDValue(L"ZipDepthMode", zipDepthMode) == ERROR_SUCCESS &&
		zipDepthMode <= ZIPDEPTH_Disable) {
		ApplyZipDepthMode(sets, static_cast<int>(zipDepthMode));
	} else {
		DWORD sr = 0, mfg = 0;
		const bool haveSr = key.QueryDWORDValue(L"UseZipDepthSr", sr) == ERROR_SUCCESS;
		const bool haveMfg = key.QueryDWORDValue(L"UseZipDepthMfg", mfg) == ERROR_SUCCESS;
		if (haveSr || haveMfg) {
			sets.bUseZipDepthSr = haveSr && sr != 0;
			sets.bUseZipDepthMfg = haveMfg && mfg != 0;
		} else if (key.QueryDWORDValue(L"UseZipDepth", dw) == ERROR_SUCCESS) {
			sets.bUseZipDepthSr = sets.bUseZipDepthMfg = dw != 0;
		} else {
			DWORD legacyMfg = 0, legacySr = 0;
			const bool haveLegacyMfg = key.QueryDWORDValue(L"MfgZipDepth", legacyMfg) == ERROR_SUCCESS;
			const bool haveLegacySr = key.QueryDWORDValue(L"DlssSRDepthMode", legacySr) == ERROR_SUCCESS;
			if (haveLegacyMfg || haveLegacySr) {
				sets.bUseZipDepthMfg = haveLegacyMfg && legacyMfg != 0;
				sets.bUseZipDepthSr = haveLegacySr && legacySr == 2;
			}
		}
	}
	if (key.QueryDWORDValue(L"SvdLutStrength", dw) == ERROR_SUCCESS && dw <= SVDLUT_STRENGTH_MAX) {
		sets.SetSvdLutStrength(static_cast<int>(dw));
	} else if (key.QueryDWORDValue(L"SvdLut", dw) == ERROR_SUCCESS) {
		sets.SetSvdLutStrength(dw ? SVDLUT_STRENGTH_MAX : 0);
	}
	if (key.QueryDWORDValue(L"JvrFlatView", dw) == ERROR_SUCCESS) sets.bJvrFlatView = dw != 0;
	if (key.QueryDWORDValue(L"JvrVerticalFraming", dw) == ERROR_SUCCESS &&
		dw <= JVR_VERTICAL_MAX - JVR_VERTICAL_MIN) {
		sets.iJvrVertical = static_cast<int>(dw) + JVR_VERTICAL_MIN;
	} else if (key.QueryDWORDValue(L"JvrVerticalPosition", dw) == ERROR_SUCCESS && dw <= 60) {
		// The first JVR build encoded [-30, 30] with a bias of 30.
		sets.iJvrVertical = static_cast<int>(dw) - 30;
	}
	if (key.QueryDWORDValue(L"JvrReprojection", dw) == ERROR_SUCCESS && dw <= JVR_REPROJECTION_MAX) {
		sets.iJvrReprojection = static_cast<int>(dw);
	}
	if (key.QueryDWORDValue(L"JvrEye", dw) == ERROR_SUCCESS && dw <= 1) {
		sets.iJvrEye = static_cast<int>(dw);
	}
	// Retired experiments are intentionally fixed off; do not restore stale values.
	sets.bMfgLowMotion = false;
	sets.bAdaptiveMfgDepth = false;
	sets.bDepth3DPreview = false;
	if (key.QueryDWORDValue(L"ShowMfgMotion", dw) == ERROR_SUCCESS) sets.bShowMfgMotion = dw != 0;
	if (key.QueryDWORDValue(L"ShowDepthOverlay", dw) == ERROR_SUCCESS) {
		sets.bShowDepthOverlay = dw != 0;
	}
	// Render ahead is set on the main page and covers the mpv prescalers as well,
	// so it is not x64 only like the rest of DLSS.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_DlssRenderAhead, dw)) {
		sets.bDlssRenderAhead = !!dw;
	}
	// Not x64 only either: these are plain resize shaders.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_UpscalingSmall, dw)) {
		sets.bUpscalingSmall = !!dw;
	}
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_UpscalingSmallFilter, dw)) {
		sets.iUpscalingSmall = discard<int>(dw, UPSCALE_Jinc2, 0, UPSCALE_COUNT-1);
	}
	// Signed, in a registry that only keeps unsigned values: it goes in as it is and
	// comes back through the same cast.
	if (ERROR_SUCCESS == key.QueryDWORDValue(OPT_VideoDelay, dw)) {
		sets.iVideoDelay = discard<int>((int)dw, 0, VIDEO_DELAY_MIN, VIDEO_DELAY_MAX);
	}
}

void WriteSettings(CRegKey& key, const Settings_t& sets)
{
	key.SetDWORDValue(OPT_VPReplaceChroma,     sets.bVPReplaceChroma);
	key.SetDWORDValue(OPT_Sharpen,             sets.iSharpen);
	key.SetDWORDValue(OPT_SharpenLevel,        sets.iSharpenLevel);
	key.SetDWORDValue(OPT_Deblock,             sets.iDeblock);
	key.SetDWORDValue(OPT_DeblockLevel,        sets.iDeblockLevel);
	key.SetDWORDValue(OPT_Deband,              sets.bDeband);
	key.SetDWORDValue(OPT_DebandLevel,         sets.iDebandLevel);
	key.SetDWORDValue(OPT_HideLogo,            sets.bHideLogo);
	key.SetDWORDValue(OPT_LogoMargin,          sets.iLogoMargin);

	key.SetDWORDValue(L"MfgMultiplier", sets.iMfgMultiplier);
	key.SetDWORDValue(L"MfgProcessingMode", sets.iMfgProcessingMode);
	key.SetDWORDValue(L"WaitForExtendedStartup", sets.bWaitForExtendedStartup);
	key.SetDWORDValue(L"MfgLowMotion", 0);
	key.SetDWORDValue(L"ZipDepthMode", ZipDepthModeFromSettings(sets));
	// Keep legacy fields synchronized so older builds/presets do not misread the mode.
	key.SetDWORDValue(L"UseZipDepth", sets.bUseZipDepthSr || sets.bUseZipDepthMfg);
	key.SetDWORDValue(L"UseZipDepthSr", sets.bUseZipDepthSr);
	key.SetDWORDValue(L"UseZipDepthMfg", sets.bUseZipDepthMfg);
	key.SetDWORDValue(L"AdaptiveMfgDepth", 0);
	key.SetDWORDValue(L"ShowMfgMotion", sets.bShowMfgMotion);
	key.SetDWORDValue(L"ShowDepthOverlay", sets.bShowDepthOverlay);
	key.SetDWORDValue(L"Depth3DPreview", 0);
	const int svdLutStrength = sets.GetSvdLutStrength();
	key.SetDWORDValue(L"SvdLutStrength", svdLutStrength);
	key.SetDWORDValue(L"SvdLut", svdLutStrength != 0);
	key.SetDWORDValue(L"JvrFlatView", sets.bJvrFlatView);
	key.SetDWORDValue(L"JvrVerticalFraming", sets.iJvrVertical - JVR_VERTICAL_MIN);
	key.SetDWORDValue(L"JvrReprojection", sets.iJvrReprojection);
	key.SetDWORDValue(L"JvrEye", sets.iJvrEye);
	key.SetDWORDValue(L"ShowFeatureBadges", sets.bShowFeatureBadges);
#ifdef _WIN64
	key.SetDWORDValue(OPT_DlssNR,              sets.bDlssNR);
	key.SetDWORDValue(OPT_DlssNRStyle,         sets.iDlssNRStyle);
	key.SetDWORDValue(OPT_DlssNRPreset,        sets.iDlssNRPreset);
	key.SetDWORDValue(OPT_DlssNRIntensity,     sets.iDlssNRIntensity);
	key.SetDWORDValue(OPT_DlssNRLocalTone,     sets.iDlssNRLocalTone);
	key.SetDWORDValue(OPT_DlssNRLocalStructure, sets.iDlssNRLocalStructure);
	key.SetDWORDValue(OPT_DlssNRSkinStructure, sets.iDlssNRSkinStructure);
	key.SetDWORDValue(OPT_DlssNRAutoMask,      sets.bDlssNRAutoMask);
	key.SetDWORDValue(OPT_DlssNRAfterUpscale,  sets.bDlssNRAfterUpscale);
	key.SetDWORDValue(OPT_DlssNRStabilizer,    sets.iDlssNRStabilizer);
	key.SetDWORDValue(OPT_DlssNRWorkingScale,  sets.iDlssNRWorkingScale);
	key.SetDWORDValue(OPT_DlssNRFlowDivider,   sets.iDlssNRFlowDivider);
	key.SetDWORDValue(OPT_DlssNRToggleKey,     sets.iDlssNRToggleKey);
	key.SetStringValue(OPT_DlssNRDllPath,      sets.szDlssNRDllPath);
	key.SetDWORDValue(OPT_DlssSR,              sets.bDlssSR);
    key.SetDWORDValue(OPT_DlssSRSourceLimit,   sets.iDlssSRSourceLimit);
	key.SetDWORDValue(OPT_DlssSRDlaa,          sets.bDlssSRDlaa);
	key.SetStringValue(OPT_DlssSRDllPath,      sets.szDlssSRDllPath);
#endif
	key.SetDWORDValue(OPT_DlssRenderAhead,     sets.bDlssRenderAhead);
	key.SetDWORDValue(OPT_UpscalingSmall,      sets.bUpscalingSmall);
	key.SetDWORDValue(OPT_UpscalingSmallFilter, sets.iUpscalingSmall);
	key.SetDWORDValue(OPT_VideoDelay,          (DWORD)sets.iVideoDelay);
}
}
