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

#include <dxva2api.h>

enum :int {
	TEXFMT_AUTOINT = 0,
	TEXFMT_8INT = 8,
	TEXFMT_10INT = 10,
	TEXFMT_16FLOAT = 16,
};

enum :int {
	DEINT_Disable = 0,
	DEINT_Enable = 1,
	DEINT_HackFutureFrames = 2,
};

enum :int {
	SUPERRES_Disable = 0,
	SUPERRES_SD,
	SUPERRES_720p,
	SUPERRES_1080p,
	SUPERRES_1440p,
	SUPERRES_Always,  // Legacy registry value; mapped to SUPERRES_1440p in settings
	SUPERRES_DLSS_SR,
	SUPERRES_COUNT
};

// DLSS SR / DLAA follows the same source-resolution gates as RTX VSR.
// The output target is always the display-fit resolution.
enum :int {
    DLSSSR_SOURCE_Disable = 0,
    DLSSSR_SOURCE_SD,
    DLSSSR_SOURCE_720p,
    DLSSSR_SOURCE_1080p,
    DLSSSR_SOURCE_1440p,
};

// Public ZipDepth routing. Internally the existing SR/MFG booleans are retained
// for Settings_t compatibility; the property page exposes only this 4-state mode.
enum :int {
    ZIPDEPTH_Both = 0,
    ZIPDEPTH_DlssSrOnly,
    ZIPDEPTH_MfgOnly,
    ZIPDEPTH_Disable,
};

enum :int {
	CHROMA_Nearest = 0,
	CHROMA_Bilinear,
	CHROMA_CatmullRom,
	CHROMA_RAVU,       // RAVU-zoom (mpv) on Cb and Cr: Direct3D 11 shaders, 4:2:0; Catmull-Rom elsewhere
	// Added after RAVU, so the numbers already saved keep their meaning. The order
	// the property page shows them in is the measured one, not this one.
	CHROMA_Jinc,       // ewa jinc, radius 3.2383, jinc-windowed: what madVR calls Jinc
	CHROMA_FSRCNNX8AR, // FSRCNNX 8 on Cb and Cr with anti-ringing; Catmull-Rom elsewhere
	CHROMA_COUNT
};

enum :int {
	UPSCALE_Nearest = 0,
	UPSCALE_Mitchell,
	UPSCALE_CatmullRom,
	UPSCALE_Lanczos2,
	UPSCALE_Lanczos3,
	UPSCALE_Jinc2,
	// mpv prescalers on luma, colour from Catmull-Rom: Direct3D 11 at feature level
	// 11.0; Catmull-Rom elsewhere and for what they leave to scale.
	UPSCALE_FSRCNNX8,
	UPSCALE_FSRCNNX16,
	UPSCALE_RAVUZoom,
	UPSCALE_FSRCNNX8AR,   // the same networks, held to the range the source covers
	UPSCALE_FSRCNNX16AR,
	UPSCALE_ArtCNN,       // ArtCNN C4F16 DS: compute passes, denoises as it enlarges
	UPSCALE_COUNT
};

enum :int {
	DOWNSCALE_Box = 0,
	DOWNSCALE_Bilinear,
	DOWNSCALE_Hamming,
	DOWNSCALE_Bicubic,
	DOWNSCALE_BicubicSharp,
	DOWNSCALE_Lanczos,
	// Appended, and it stays last: the Downscaling list saves the position it sits
	// on rather than a value of its own, so inserting above would move everybody's
	// saved setting one along.
	DOWNSCALE_Spline36,
	DOWNSCALE_COUNT
};

enum :int {
	SWAPEFFECT_Discard = 0,
	SWAPEFFECT_Flip,
	SWAPEFFECT_COUNT
};

enum :int {
	HDRTD_Disabled = 0,
	HDRTD_On_Fullscreen,
	HDRTD_On,
	HDRTD_OnOff_Fullscreen,
	HDRTD_OnOff
};

#define SDR_NITS_DEF 125
#define SDR_NITS_MIN  25
#define SDR_NITS_MAX 400
#define SDR_NITS_STEP  5

// HDR to SDR conversion method
enum :int {
	SDRTM_Fixed = 0, // fixed curve
	SDRTM_Metadata,  // curve built from the HDR metadata and the SDR display nits
	SDRTM_Measured,  // the same curve, from the peak measured in each frame
	SDRTM_COUNT
};

// Tuning of SDRTM_Measured. Registry only: SdrPeakWindowMs, SdrPeakFloorNits and SdrPeakSceneCuts.
#define SDR_PEAK_WINDOW_DEF 3000 // ms
#define SDR_PEAK_WINDOW_MIN    0
#define SDR_PEAK_WINDOW_MAX 10000
#define SDR_PEAK_FLOOR_DEF   640 // nits
#define SDR_PEAK_FLOOR_MIN     0
#define SDR_PEAK_FLOOR_MAX 10000

constexpr inline auto HDR_NITS_DEF = 1000;
constexpr inline auto HDR_NITS_MIN = 100;
constexpr inline auto HDR_NITS_MAX = 10000;

#include "Edition/SettingsValues.h"
struct VPEnableFormats_t {
	bool bNV12;
	bool bP01x;
	bool bYUY2;
	bool bOther;
};

struct Settings_t {
	bool bUseD3D11;
	bool bShowStats;
	int  iResizeStats;
	int  iTexFormat;
	VPEnableFormats_t VPFmts;
	int  iVPDeinterlacing;
	bool bDeintDouble;
	bool bVPScaling;
	int iVPSuperRes;
	bool bVPRTXVideoHDR;
	int  iChromaScaling;
	int  iUpscaling;
	int  iDownscaling;
	bool bInterpolateAt50pct;
	bool bUseDither;
	bool bDeintBlend;
	int  iSwapEffect;
	bool bExclusiveFS;
	bool bVBlankBeforePresent;
	bool bAdjustPresentTime;
	bool bReinitByDisplay;
	bool bHdrPreferDoVi;
	bool bHdrPassthrough;
	int  iHdrToggleDisplay;
	int  iHdrOsdBrightness;
	bool bConvertToSdr;
	int  iSdrToneMappingType;
	int  iSdrPeakWindowMs;
	int  iSdrPeakFloorNits;
	bool bSdrPeakSceneCuts;
	int  iSDRDisplayNits;
	bool bHdrLocalToneMapping;
	int  iHdrLocalToneMappingType;
	bool bHdrLocalMeasurePeak;
	int  iHdrDoviToneMappingType; // 6 = ST 2094-10, 0 = the HDR method
	int iHdrDisplayMaxNits;

#include "Edition/SettingsExtension.inc"

	Settings_t() {
		SetDefault();
	}

	void SetDefault() {
		SetEditionDefaults();
		if (IsWindows8OrGreater()) {
			bUseD3D11                   = true;
		} else {
			bUseD3D11                   = false;
		}
		bShowStats                      = false;
		iResizeStats                    = 0;
		iTexFormat                      = TEXFMT_AUTOINT;
		VPFmts.bNV12                    = true;
		VPFmts.bP01x                    = true;
		VPFmts.bYUY2                    = true;
		VPFmts.bOther                   = true;
		iVPDeinterlacing                = DEINT_Enable;
		bDeintDouble                    = true;
		bVPScaling                      = true;
		iVPSuperRes                     = SUPERRES_Disable;
		bVPRTXVideoHDR                  = false;
		iChromaScaling                  = CHROMA_CatmullRom;
		iUpscaling                      = UPSCALE_Jinc2;
		iDownscaling                    = DOWNSCALE_Hamming;
		bInterpolateAt50pct             = true;
		bUseDither                      = true;
		bDeintBlend                     = false;
		iSwapEffect                     = SWAPEFFECT_Flip;
		bExclusiveFS                    = false;
		bVBlankBeforePresent            = false;
		bAdjustPresentTime              = true;
		bReinitByDisplay                = false;
		bHdrPreferDoVi                  = false;
		if (IsWindows10OrGreater()) {
			bHdrPassthrough             = true;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			bHdrLocalMeasurePeak        = false;
			iHdrDoviToneMappingType     = 6;
			iHdrDisplayMaxNits          = 1000;
		} else {
			bHdrPassthrough             = false;
			bHdrLocalToneMapping        = false;
			iHdrLocalToneMappingType    = 1;
			bHdrLocalMeasurePeak        = false;
			iHdrDoviToneMappingType     = 6;
			iHdrDisplayMaxNits          = 1000;
		}
		iHdrToggleDisplay               = HDRTD_Disabled;
		bConvertToSdr                   = true;
		iSdrToneMappingType             = SDRTM_Metadata;
		iSdrPeakWindowMs                = SDR_PEAK_WINDOW_DEF;
		iSdrPeakFloorNits               = SDR_PEAK_FLOOR_DEF;
		bSdrPeakSceneCuts               = false;
		iHdrOsdBrightness               = 0;
		iSDRDisplayNits                 = SDR_NITS_DEF;
	}
};

#include "Edition/SettingsPresets.inc"
// This fork extends Settings_t with iMfgMultiplier. Keep its private IID:
// claiming the upstream IID without preserving the original ABI is unsafe.
interface __declspec(uuid("8CB7C87F-E8C6-4650-BA0D-978E863D983A"))
IVideoRenderer : public IUnknown {
	STDMETHOD(GetVideoProcessorInfo) (std::wstring& str) PURE;
	STDMETHOD_(bool, GetActive()) PURE;

	STDMETHOD_(void, GetSettings(Settings_t& setings)) PURE;
	STDMETHOD_(void, SetSettings(const Settings_t& setings)) PURE;

	STDMETHOD(SaveSettings()) PURE;

	// Everything of ours goes after everything of upstream's, and stays there. The
	// interface keeps upstream's identifier, so a caller built against upstream's
	// header finds each method by its place in the table: one of ours put in the
	// middle moves all of upstream's that follow it, and the caller then asks for
	// the settings and saves them instead. Measured, when this was in the middle:
	// setting the options on a stock build did nothing at all and wrote its defaults
	// to the registry.
	STDMETHOD_(unsigned, GetVideoProcessorUse()) PURE;

	// Presets: PRESET_COUNT whole sets of settings kept beside the ones in force,
	// each with a name and a key that recalls it while a film plays. LoadPreset
	// answers S_FALSE for a slot nothing was saved in, and so does GetPresetInfo,
	// which still hands back the name and the key the slot was given.
	STDMETHOD(SavePreset(int slot, const Settings_t& sets, LPCWSTR name, int vk)) PURE;
	STDMETHOD(LoadPreset(int slot, Settings_t& sets)) PURE;
	STDMETHOD(GetPresetInfo(int slot, LPWSTR name, int cchName, int* pVk)) PURE;
	STDMETHOD(SetPresetInfo(int slot, LPCWSTR name, int vk)) PURE;
};
