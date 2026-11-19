// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

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

