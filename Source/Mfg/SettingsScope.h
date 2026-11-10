// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../IVideoRenderer.h"
namespace mfg {
// Enforce only retired Plus-only functions and MFG range: NEVER reset the native
// MPC Video Renderer settings (HDR, shader scaling, RTX VSR, D3D11, deinterlacing).
// The upstream Main property page owns those fields and must remain authoritative.
inline void EnforceScope(Settings_t& s) {
    s.iMfgMultiplier = (s.iMfgMultiplier >= 2 && s.iMfgMultiplier <= 7) ? 7u : 0u;
    // Feature backends removed from this fork are not independently selectable.
    s.bDlssNR = false;
    s.bDlssSR = false;
    s.bDlssSRDlaa = false;
    s.iDlssNRToggleKey = 0;
    s.bVPReplaceChroma = false;
    s.iSharpen = SHARPEN_Disabled;
    s.iDeblock = DEBLOCK_Off;
    s.bDeband = false;
    s.bHideLogo = false;
    // Older Plus settings may contain shader modes not exposed by original MPC-VR.
    if (s.iUpscaling > UPSCALE_Jinc2 || s.iUpscaling < UPSCALE_Nearest) s.iUpscaling = UPSCALE_CatmullRom;
    if (s.iUpscalingSmall > UPSCALE_Jinc2 || s.iUpscalingSmall < UPSCALE_Nearest) s.iUpscalingSmall = UPSCALE_CatmullRom;
    if (s.iChromaScaling > CHROMA_CatmullRom || s.iChromaScaling < CHROMA_Nearest) s.iChromaScaling = CHROMA_CatmullRom;
    if (s.iDownscaling > DOWNSCALE_Lanczos || s.iDownscaling < DOWNSCALE_Box) s.iDownscaling = DOWNSCALE_Hamming;
    // Do NOT force video-processor scaling or change source format checkboxes
    // when RTX VSR is selected; those remain user-controlled upstream settings.
}
}
