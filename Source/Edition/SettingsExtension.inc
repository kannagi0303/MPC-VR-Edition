// SPDX-License-Identifier: GPL-3.0-or-later
// Inside Settings_t, immediately after its native members. Preserve field order/ABI.
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
	bool bShowEditionDetails = false; // Uses the alignment byte before iDlssSRSourceLimit.
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

	int GetSvdLutStrength() const {
		if (!bSvdLut) return 0;
		return svdLutStrength > 0 && svdLutStrength <= SVDLUT_STRENGTH_MAX
			? svdLutStrength : 100; // legacy fallback must remain 100%, not new 200%
	}

	void SetSvdLutStrength(int strength) {
		svdLutStrength = static_cast<BYTE>(std::clamp(strength, 0, SVDLUT_STRENGTH_MAX));
		bSvdLut = svdLutStrength != 0;
	}

	void SetEditionDefaults() {
		iMfgMultiplier = 0;
		iMfgProcessingMode = 0;
		bWaitForExtendedStartup = false;
		bShowFeatureBadges = true;
		bShowEditionDetails = false;
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
		bVPReplaceChroma                = false;
		iSharpen                        = SHARPEN_Disabled;
		iSharpenLevel                   = SHARPEN_LEVEL_DEF;
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
