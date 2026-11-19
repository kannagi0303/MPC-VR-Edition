// SPDX-License-Identifier: GPL-3.0-or-later
// After Settings_t; shared preset field policies. Keep declaration order; this fragment is not a standalone header.
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

