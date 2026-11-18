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

#include <algorithm>
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

enum :int {
	DLSSNR_STYLE_Default = 0,
	DLSSNR_STYLE_Natural,
	DLSSNR_STYLE_Cinematic,
	DLSSNR_STYLE_COUNT
};

constexpr inline auto DLSSNR_PRESET_COUNT = 4;
// Strength sliders: stored and edited as ints, divided by DLSSNR_STR_SCALE
// before being handed to NGX.
constexpr inline auto DLSSNR_STR_SCALE = 100;
constexpr inline auto DLSSNR_STR_MIN   = 0;
constexpr inline auto DLSSNR_STR_MAX   = 200;
constexpr inline auto DLSSNR_STR_DEF   = 100;
constexpr inline auto DLSSNR_SKIN_MIN  = -100;
// Temporal stabilizer, 0..100: how strongly the network's effect is steadied over
// time after it runs. 0 runs nothing; 100 is the measured setting.
constexpr inline auto DLSSNR_STAB_MIN  = 0;
constexpr inline auto DLSSNR_STAB_MAX  = 100;
constexpr inline auto DLSSNR_STAB_DEF  = 100;
// What the picture is divided by for the stabilizer's NVIDIA Optical Flow: 4, 3, 2,
// or 1 for the working size itself. Auto takes 1, 2 or 4 from the working height for
// about 540 lines, as the stabilizer always did: 960x540 for a 1080p film, and for a
// 4K one at its own size.
constexpr inline auto DLSSNR_FLOW_AUTO = 0;
constexpr inline auto DLSSNR_FLOW_MAX  = 4;
constexpr inline auto DLSSNR_FLOW_DEF  = DLSSNR_FLOW_AUTO;
// The size DLSS 5 NR works at, per cent of the picture's each way. The network costs
// in proportion to the pixels it is given; below 100 it runs on the picture reduced,
// and the change it made is carried back onto the picture at its own size
// (CDlssCompose). The step is the slider's.
constexpr inline auto DLSSNR_SCALE_MIN  = 50;
constexpr inline auto DLSSNR_SCALE_MAX  = 100;
constexpr inline auto DLSSNR_SCALE_STEP = 5;
constexpr inline auto DLSSNR_SCALE_DEF  = 100;

// Names shown in the playback badge for the model requested from NGX.
constexpr inline auto DLSSSR_PRESET_J   = 10;
constexpr inline auto DLSSSR_PRESET_K   = 11;
constexpr inline auto DLSSSR_PRESET_L   = 12;
constexpr inline auto DLSSSR_PRESET_M   = 13;

constexpr const wchar_t* DlssSRModelLabel(int preset)
{
	switch (preset) {
	case DLSSSR_PRESET_J: return L"J (Alt. Quality)";
	case DLSSSR_PRESET_K: return L"K (Quality)";
	case DLSSSR_PRESET_L: return L"L (Ultra Perf.)";
	case DLSSSR_PRESET_M: return L"M (Performance)";
	default: return L"Auto";
	}
}

struct VPEnableFormats_t {
	bool bNV12;
	bool bP01x;
	bool bYUY2;
	bool bOther;
};

// Sharpening, after the resize. Adaptive-sharpen is the only one that does not amplify
// grain, and the unsharp mask held to what its neighbours cover is five times cheaper and
// rings less on a clean picture; AMD's CAS sits between
// them and is the one asked for by name. Appended: the list saves its position.
enum : int {
    SHARPEN_Disabled = 0,
    SHARPEN_Adaptive,
    SHARPEN_UnsharpClamp,
    SHARPEN_CAS,
    SHARPEN_COUNT
};

// Five levels, and the same level means the same thing whichever method runs:
// Adaptive-Sharpen's are the anchor, and the others give, on the same chain, the
// gain in mean gradient it gives (playback_test --sharpencal). Level 3 is where the
// methods peak against the reference.
constexpr inline auto SHARPEN_LEVEL_MIN = 1;
constexpr inline auto SHARPEN_LEVEL_MAX = 5;
constexpr inline auto SHARPEN_LEVEL_DEF = 3;

// Deblocking and debanding on the decoded planes, five levels each: what each level
// does is in Source/Clean/CleanParams.h, which the measurement tools read as well.
// The deblocking has two methods: PP7, light, and SPP over 8 grids, three times the
// cost and closer to the clean picture. The values are Clean::DeblockMethod's.
enum : int {
    DEBLOCK_Off = 0,
    DEBLOCK_PP7,
    DEBLOCK_SPP,
    DEBLOCK_COUNT
};
constexpr inline auto CLEAN_LEVEL_MIN = 1;
constexpr inline auto CLEAN_LEVEL_MAX = 5;
constexpr inline auto CLEAN_LEVEL_DEF = 3;
// The margin round a channel's logo worked with it, in luma pixels (Logo::kMargin*).
constexpr inline auto LOGO_MARGIN_MIN = 0;
constexpr inline auto LOGO_MARGIN_MAX = 3;
constexpr inline auto LOGO_MARGIN_DEF = 1;

// The picture delay, in milliseconds. Later is what a slow sound asks for; earlier is
// there for a television that takes longer over the picture than the sound takes to
// arrive. The step is the slider's: a picture moves by whole refreshes in any case.
constexpr inline auto VIDEO_DELAY_MIN  = -200;
constexpr inline auto VIDEO_DELAY_MAX  = 500;
constexpr inline auto VIDEO_DELAY_STEP = 5;

inline constexpr int JVR_VERTICAL_MIN = -60;
inline constexpr int JVR_VERTICAL_MAX = 60;
inline constexpr int JVR_VERTICAL_DEFAULT = 0;
inline constexpr int JVR_REPROJECTION_MIN = 0;
inline constexpr int JVR_REPROJECTION_MAX = 200;
inline constexpr int JVR_REPROJECTION_DEFAULT = 50;
inline constexpr int SVDLUT_STRENGTH_MAX = 100;

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
	// DLSS 5 Neural Rendering. Strengths are stored x100 so the whole struct
	// stays integral and round-trips through the DWORD-only registry.
	bool bDlssNR;
	int  iDlssNRStyle;
	int  iDlssNRPreset;
	int  iDlssNRIntensity;
	int  iDlssNRLocalTone;
	int  iDlssNRLocalStructure;
	int  iDlssNRSkinStructure;
	bool bDlssNRAutoMask;
	// Retired, and kept where it is, Settings_t being append-only: nothing reads it.
	// It forced DLSSNR.Reset on every picture; the network's own blend with its last
	// output measures the same as no history, and the box is gone from the page.
	bool bDlssNRNoHistory;
	// Temporal stabilizer after the network, DLSSNR_STAB_*; 0 = off.
	int  iDlssNRStabilizer;
	// Retired the same way: the stabilizer's motion source, when a shader detector
	// could stand in for NVIDIA Optical Flow. Optical Flow is the only one left.
	int  iDlssNRMotion;
	// Retired the same way: whether the stabilizer's Optical Flow vectors also went to
	// the network. They always do now, cleaned the way DLSS SR's are, while the
	// stabilizer runs.
	bool bDlssNRMotionVectors;
	// Run the pass at display resolution, after scaling, instead of at source
	// resolution before it. Much heavier at 4K.
	bool bDlssNRAfterUpscale;
	// Virtual-key code that toggles DLSS during playback, 0 = no key.
	int  iDlssNRToggleKey;
	wchar_t szDlssNRDllPath[MAX_PATH];
	// DLSS Super Resolution in place of the Upscaling method, set on the DLSS 5
	// page and independent of DLSS 5 NR: nvngx_dlss.dll through the display
	// driver's NGX runtime.
	bool bDlssSR;
	int reservedDlssSRPreset = {}; // Legacy Settings_t slot.
	wchar_t szDlssSRDllPath[MAX_PATH];
	// Start the DLSS passes early by what they take, and hold each finished picture
	// until its time, so DLSS does not make the video late (see CRenderAhead).
	bool bDlssRenderAhead;

	// Added after everything upstream has, and it stays there: the video processor's
	// own chroma upsampling measures about bilinear, and on a 10-bit source the driver
	// reads the studio range as 16/255..235/255 whatever the depth, which shifts the
	// colour by about one 8-bit level. With this, the shaders rebuild the chroma of a
	// progressive 4:2:0 or 4:2:2 picture with iChromaScaling and hand the processor a
	// 4:4:4 one. Interlaced video keeps the processor's own chroma: it is the only
	// deinterlacer there is, and it deinterlaces 4:2:0 only. Direct3D 11 only.
	bool bVPReplaceChroma;

	// Sharpening after the resize: SHARPEN_*, and which of the five levels.
	int iSharpen;
	int iSharpenLevel;

	// DLSS Super Resolution at the picture's own size: the network runs and nothing
	// is enlarged, which is what games call DLAA. Only reached when the picture is
	// already exactly the size it is shown at, so it costs nothing anywhere else.
	bool bDlssSRDlaa;

	// Another Upscaling method for an enlargement under five per cent -- a 4K film
	// cropped to 3838 across on a 3840 screen. A network doubles its source whatever
	// the target is, so there it costs a whole pass at twice the screen's size for
	// nothing anybody can see: FSRCNNX 16 measured 398.7 ms a frame.
	bool bUpscalingSmall;
	int  iUpscalingSmall;   // UPSCALE_*

	// Every picture shown this many milliseconds after its time, VIDEO_DELAY_*: for a
	// sound that reaches the ear late. The picture is all a renderer can move, and an
	// amplifier that decodes behind the television is seen by nothing in the player.
	int  iVideoDelay;

	// DLSS 5 NR at this share of the picture's size, DLSSNR_SCALE_*: 100 runs it at
	// the size itself. On the DLSS 5 page, so CopyDlssSettings carries it.
	int  iDlssNRWorkingScale;

	// What the working picture is divided by for the stabilizer's Optical Flow,
	// DLSSNR_FLOW_*, 0 being Auto. DLSS Super Resolution keeps its own Auto.
	int  iDlssNRFlowDivider;

	// Artifact removal on the decoded planes, before anything converts or resizes the
	// picture: deblocking (the codec's 8 x 8 blocks and its ringing) and debanding (the
	// steps it leaves in smooth gradients), CLEAN_LEVEL_* each. Progressive planar
	// video, Direct3D 11; RTX Video Super Resolution, which cleans the picture itself,
	// leaves them out (Greying.h). iDeblock is the method, DEBLOCK_*, 0 for off.
	int  iDeblock;
	int  iDeblockLevel;
	bool bDeband;
	int  iDebandLevel;

	// A channel's logo hidden: found by "Find the logo" on the settings page, or
	// recognised from the logos kept, and taken out of the decoded planes before the
	// deblocking. Progressive two-plane video, Direct3D 11; RTX Video Super Resolution
	// leaves it on, as it does not hide logos.
	bool bHideLogo;
	// The pixels round the logo worked with it, LOGO_MARGIN_*: for an edge the encoder
	// smeared further than the analysis saw.
	int  iLogoMargin;
	// MPC-VR-MFG private settings: Off=0, Auto=7 (legacy x2..x6 map to Auto).
	unsigned iMfgMultiplier = 0;
	// Kept at the end of Settings_t to preserve previous members and presets.
	// Hybrid MFG processing order, independent of the final D3D11 Present.
	unsigned iMfgProcessingMode = 0; // 0 Quality: VSR -> MFG; 1 Performance: MFG -> VSR
	bool bShowFeatureBadges = true; // show DLSS SR, VSR and MFG status on the video
	BYTE svdLutStrength = 0; // Reuses the retired processing-resolution slot.
	bool reservedDlssSRValidateMotion = {}; // Legacy Settings_t slot.
    int iDlssSRSourceLimit = DLSSSR_SOURCE_Disable; // Appended: preserve the existing settings layout.
	// Reserved legacy A/B slot, kept for Settings_t layout/preset compatibility.
	// No GUI or runtime toggle; normalization always forces true.
	bool bDlssSRTemporalHistory = true;
	int reservedDlssSRMotionQuality = {}; // Legacy Settings_t slot.
	bool bSvdLut = false; // Legacy enable flag, also read by older callers.
	int reservedDlssSRDepthMode = {}; // Legacy Settings_t slot.
	bool bWaitForExtendedStartup = false; // Reuses the retired pulldown slot.
	bool bMfgLowMotion = false;
	bool bUseZipDepthSr = true;
	unsigned char reservedMfgOriginalProtection = {}; // Legacy Settings_t slot.
	bool bShowDepthOverlay = false;
	bool bUseZipDepthMfg = true; // Reuses the retired one-byte opacity slot.
	bool bAdaptiveMfgDepth = false;
	bool bShowMfgMotion = false;
	bool bDepth3DPreview = false; // Experimental ZipDepth-driven oblique parallax preview.
	bool bJvrFlatView = false;
	int iJvrVertical = JVR_VERTICAL_DEFAULT;
	int iJvrReprojection = JVR_REPROJECTION_DEFAULT;
	int iJvrProfile = 0; // Reserved, keeps the Settings_t interface layout stable.
	int iJvrEye = 0;

	Settings_t() {
		SetDefault();
	}

	int GetSvdLutStrength() const {
		if (!bSvdLut) return 0;
		return svdLutStrength > 0 && svdLutStrength <= SVDLUT_STRENGTH_MAX
			? svdLutStrength : SVDLUT_STRENGTH_MAX;
	}

	void SetSvdLutStrength(int strength) {
		svdLutStrength = static_cast<BYTE>(std::clamp(strength, 0, SVDLUT_STRENGTH_MAX));
		bSvdLut = svdLutStrength != 0;
	}

	void SetDefault() {
		iMfgMultiplier = 0;
		iMfgProcessingMode = 0;
		bWaitForExtendedStartup = false;
		bShowFeatureBadges = true;
        iDlssSRSourceLimit = DLSSSR_SOURCE_Disable;
		bDlssSRTemporalHistory = true;
		SetSvdLutStrength(0);
		bMfgLowMotion = false;
		bUseZipDepthSr = true;
		bUseZipDepthMfg = true;
		bAdaptiveMfgDepth = false;
		bShowMfgMotion = false;
		bShowDepthOverlay = false;
		bDepth3DPreview = false;
		bJvrFlatView = false;
		iJvrVertical = JVR_VERTICAL_DEFAULT;
		iJvrReprojection = JVR_REPROJECTION_DEFAULT;
		iJvrProfile = 0;
		iJvrEye = 0;
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
		bVPReplaceChroma                = false;
		iSharpen                        = SHARPEN_Disabled;
		iSharpenLevel                   = SHARPEN_LEVEL_DEF;
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
		bDlssNR                         = false;
		iDlssNRStyle                    = DLSSNR_STYLE_Default;
		iDlssNRPreset                   = 0;
		// Tuned by ear on real video rather than left at neutral: the local
		// terms are the ones that amplify frame-to-frame variation, so they sit
		// well below 1.00 while overall intensity sits above it.
		iDlssNRIntensity                = 150;   // 1.50
		iDlssNRLocalTone                = 30;    // 0.30
		iDlssNRLocalStructure           = 50;    // 0.50
		iDlssNRSkinStructure            = 90;    // 0.90
		bDlssNRAutoMask                 = true;
		bDlssNRNoHistory                = false;
		bDlssNRAfterUpscale             = false;
		iDlssNRStabilizer               = DLSSNR_STAB_DEF;
		iDlssNRMotion                   = 1;   // retired: Optical Flow, as it always is now
		bDlssNRMotionVectors            = false;
		iDlssNRToggleKey                = VK_F12;
		szDlssNRDllPath[0]              = L'\0';
		bDlssSR                         = false;
		bDlssSRDlaa                     = false;
		bUpscalingSmall                 = false;
		iUpscalingSmall                 = UPSCALE_Jinc2;
		szDlssSRDllPath[0]              = L'\0';
		bDlssRenderAhead                = true;
		iVideoDelay                     = 0;
		iDlssNRWorkingScale             = DLSSNR_SCALE_DEF;
		iDlssNRFlowDivider              = DLSSNR_FLOW_DEF;
		iDeblock                        = DEBLOCK_Off;
		iDeblockLevel                   = CLEAN_LEVEL_DEF;
		bDeband                         = false;
		iDebandLevel                    = CLEAN_LEVEL_DEF;
		bHideLogo                       = false;
		iLogoMargin                     = LOGO_MARGIN_DEF;
	}
};

// Every field of the DLSS 5 page, DLSS Super Resolution included, for the
// property pages that must not overwrite each other: the main page takes these
// from the renderer before applying.
inline void CopyDlssSettings(Settings_t& dst, const Settings_t& src)
{
	dst.bDlssNR               = src.bDlssNR;
	dst.iDlssNRStyle          = src.iDlssNRStyle;
	dst.iDlssNRPreset         = src.iDlssNRPreset;
	dst.iDlssNRIntensity      = src.iDlssNRIntensity;
	dst.iDlssNRLocalTone      = src.iDlssNRLocalTone;
	dst.iDlssNRLocalStructure = src.iDlssNRLocalStructure;
	dst.iDlssNRSkinStructure  = src.iDlssNRSkinStructure;
	dst.bDlssNRAutoMask       = src.bDlssNRAutoMask;
	dst.iDlssNRStabilizer     = src.iDlssNRStabilizer;
	dst.bDlssNRAfterUpscale   = src.bDlssNRAfterUpscale;
	dst.iDlssNRWorkingScale   = src.iDlssNRWorkingScale;
	dst.iDlssNRFlowDivider    = src.iDlssNRFlowDivider;
	dst.iDlssNRToggleKey      = src.iDlssNRToggleKey;
	wcscpy_s(dst.szDlssNRDllPath, src.szDlssNRDllPath);
	dst.bDlssSR               = src.bDlssSR;
    dst.iDlssSRSourceLimit     = src.iDlssSRSourceLimit;
	dst.bDlssSRDlaa           = src.bDlssSRDlaa;
	dst.bDlssSRTemporalHistory = src.bDlssSRTemporalHistory;
	wcscpy_s(dst.szDlssSRDllPath, src.szDlssSRDllPath);
	// bDlssRenderAhead is not here: it belongs to the main page, which sets it for
	// the prescalers as much as for DLSS.
}

// What a preset does not carry, because it belongs to the installation, the room or the
// screen rather than to a taste: where the two DLLs are, the DLSS toggle key, how late the
// room's sound is, the Measured method's tuning (registry only, as upstream has it),
// whether the statistics are up, and the whole of the Presentation tab. Presets saved by
// earlier versions hold a Discard swap effect and the wait for a vertical blank, and
// recalling one turned the scheduled presentation off for every SDR picture.
inline void KeepOutOfPreset(Settings_t& sets, const Settings_t& current)
{
	sets.bJvrFlatView          = current.bJvrFlatView;
	sets.iJvrVertical         = current.iJvrVertical;
	sets.iJvrReprojection     = current.iJvrReprojection;
	sets.iJvrProfile          = current.iJvrProfile;
	sets.iJvrEye              = current.iJvrEye;
	sets.iDlssNRToggleKey     = current.iDlssNRToggleKey;
	sets.iSdrPeakWindowMs     = current.iSdrPeakWindowMs;
	sets.iSdrPeakFloorNits    = current.iSdrPeakFloorNits;
	sets.bSdrPeakSceneCuts    = current.bSdrPeakSceneCuts;
	wcscpy_s(sets.szDlssNRDllPath, current.szDlssNRDllPath);
	wcscpy_s(sets.szDlssSRDllPath, current.szDlssSRDllPath);
	sets.iVideoDelay          = current.iVideoDelay;
	sets.bShowStats           = current.bShowStats;
	sets.iSwapEffect          = current.iSwapEffect;
	sets.bExclusiveFS         = current.bExclusiveFS;
	sets.bVBlankBeforePresent = current.bVBlankBeforePresent;
	sets.bAdjustPresentTime   = current.bAdjustPresentTime;
	sets.bReinitByDisplay     = current.bReinitByDisplay;
	sets.bDlssRenderAhead     = current.bDlssRenderAhead;
}

constexpr int PRESET_COUNT = 10;

// The key that recalls a preset: a virtual key in the low byte and, above it, the
// keys that must be held with it. Ctrl+1 and 1 are two different keys.
constexpr int PRESETKEY_Shift = 0x100;
constexpr int PRESETKEY_Ctrl  = 0x200;
constexpr int PRESETKEY_Alt   = 0x400;
constexpr int PRESETKEY_Mask  = 0x7FF;

// What the hardware video processor is doing with the picture being played, which
// decides whether the shader lists have anything to do: it converts the formats it
// was given, chroma upsampling included, and resizes when it was told to and no DLSS
// pass has taken the resizing back from it.
enum :unsigned {
	VPUSE_Converting = 1,
	VPUSE_Resizing   = 2,
	// Dolby Vision is the one thing that keeps the processor out of the chain
	// altogether rather than merely idle: the picture carries Dolby Vision's own
	// colour and its own reshaping, which a fixed-function processor cannot convert,
	// so the shaders take all of it. Reported whether or not the processor is up,
	// because the page has to tell "out of the chain" from "nothing to do".
	VPUSE_DoViByShaders = 4,
};

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
