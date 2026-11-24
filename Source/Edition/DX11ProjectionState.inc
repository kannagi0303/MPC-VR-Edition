// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CDX11VideoProcessor projection/color state. Keep declaration order; this fragment is not a standalone header.
	bool m_jvrFlatView = false;
	int m_jvrEye = 0;
	jvr::ViewTransform m_jvrView;
	std::array<float, 16> m_jvrUploadedConstants{};
	struct SvdLutScene {
		REFERENCE_TIME start;
		bool submitted = false;
		bool fadeIn = false;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
	};
	struct {
		svdlut::ModelWorker model;
		std::unique_ptr<svdlut::Image> input;
		std::deque<SvdLutScene> scenes;
		std::array<float, 8 * 8 * 3> signature{};
		std::array<float, 8 * 8 * 3> cutSignature{};
		REFERENCE_TIME captured = INVALID_TIME;
		REFERENCE_TIME possibleCut = INVALID_TIME;
		REFERENCE_TIME inference = INVALID_TIME;
		REFERENCE_TIME transitionStart = INVALID_TIME;
		REFERENCE_TIME nextCapture = INVALID_TIME;
		int strength = 0;
		bool attempted = false;
		bool failed = false;
		bool hasSignature = false;
		bool active = false;
		bool waitingForBuffer = false;
		bool clearViewSupported = false;
		Tex2D_t picture, nativeThumbnail, thumbnail, staging;
		CComPtr<ID3D11SamplerState> lutSampler;
		CComPtr<ID3D11Buffer> constants;
	} m_svdLut;
	// Shared preprocessing shader, independent of either model worker.
	CComPtr<ID3D11PixelShader> m_svdLutThumbnailShader;
	CComPtr<ID3D11Buffer> m_svdLutThumbnailConstants;

	// Experimental RAW B is intentionally independent of the production
	// scene-based SVDLUT worker, thumbnails, grade tables and statistics.
	struct SvdLutRawFrame {
		REFERENCE_TIME pts = INVALID_TIME;
		uint64_t fence = 0;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view, temporalView;
        // B2 metadata only; never used to change B1 selection or regression.
        std::shared_ptr<const svdlut::Parameters> temporalParameters;
		std::shared_ptr<const svdlut::Parameters> parameters;
		std::array<float, 8 * 8 * 3> signature{};
        // Refined B2.1 samples from the same original 256x256 thumbnail map.
        std::array<float, 16 * 16 * 2 * 3> b2Samples{};
        bool b2SamplesReady = false;
		svdlut::temporal::Features colorFeatures{};
		bool signatureReady = false;
		bool temporalFinalized = false;
	};
	// Only Temporal B uses these asynchronous readback slots. Map(DO_NOT_WAIT)
	// never stalls the presenter. Resources are owned by the renderer thread.
	struct SvdLutTemporalCapture {
		Tex2D_t staging;
		REFERENCE_TIME pts = INVALID_TIME;
		uint64_t fence = 0;
		bool pending = false;
        // Readback is independent of an already running DirectML inference.
        // A CPU-ready thumbnail must not wait for the worker before the
        // original-frame scene signature becomes available.
        std::unique_ptr<svdlut::Image> input;
	};
	struct {
		svdlut::ModelWorker model;
		std::array<SvdLutRawFrame, 16> frames;
		std::array<SvdLutTemporalCapture, 3> captures;
		size_t frameSlot = 0, captureSlot = 0;
		bool inFlight = false;
		REFERENCE_TIME inFlightPts = INVALID_TIME;
		uint64_t inFlightFence = 0, inFlightStart = 0;
        // Keep the latest completed LUT independent of the 16-frame ring.
        // Otherwise a slow result can be lost when its source frame is evicted.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> heldView;
        std::shared_ptr<const svdlut::Parameters> heldParameters;
        std::array<float, 8 * 8 * 3> heldSignature{}, inFlightSignature{};
        REFERENCE_TIME heldPts = INVALID_TIME;
        bool heldSignatureReady = false;
        uint64_t appliedDraws = 0, missedResult = 0;
        // B2.1 is an optional, strictly post-B1 scene-brightness correction.
        // Only B2 uses these fields; temporal LUT regression remains untouched.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> b2CachedView;
        REFERENCE_TIME b2CachedSourcePts = INVALID_TIME;
        bool b2CachedRefinedReady = false; // upgrade legacy cache after thumbnail arrives
        // Replicated B2.1 luma participation, never mixed into B1 LUT.
        std::array<float, 3> b2CachedTone{0.0f, 0.0f, 0.0f};
        std::array<float, 3> b2LastTone{0.0f, 0.0f, 0.0f};
        // B2-only shot anchor, advanced once per new ORIGINAL-frame PTS.
        std::array<float, 8 * 8 * 3> b2ShotSignature{};
        REFERENCE_TIME b2ShotPts = INVALID_TIME;
        bool b2ShotValid = false;
        float b2LastSourceShiftEv = 0.0f, b2LastResidualEv = 0.0f;
        unsigned b2LastSamples = 0;
        // SVDLUT audit: one synthetic neutral-gray input sampled through the
        // *selected* packed LUT (CPU only, no texture readback or GPU work).
        // Diagnostic values do NOT include B2.1/P1/P2 or the display pipeline.
        struct GrayProbe {
            std::array<float, 3> model{}, strengthMixed{};
            REFERENCE_TIME pts = INVALID_TIME;
            unsigned source = 0; // 0 raw, 1 temporal view, 2 held/replayed
            bool valid = false;
        } grayProbe;
        uint64_t b2Analyses = 0, b2AdjustedDraws = 0;
        uint64_t b2AnalysisTicks = 0, b2BudgetSkips = 0, b2ShotResets = 0;
        bool b2ShaderAttempted = false, b2ShaderFailed = false;
        bool b2MemoryShaderAttempted = false, b2MemoryShaderFailed = false;
        bool r1GamutShaderAttempted = false, r1GamutShaderFailed = false;
        // Retained M1/R1/D1 legacy experiment state. P4 gray-probe must not
        // replace fields consumed by SvdLutRawMode.inc.
        bool m1ShaderAttempted = false, m1ShaderFailed = false;
        bool r1M1ShaderAttempted = false, r1M1ShaderFailed = false;
        uint64_t m1SkinDraws = 0;
        uint64_t d1DepthDraws = 0, d1FallbackDraws = 0;
        uint64_t r1GamutDraws = 0; // alternative to B2.2, not an extra pass
        uint64_t b2MemoryDraws = 0; // selected shader draws, not classified pixels
        // Keep the actual successfully presented B2 transform when pausing.
        // The original-frame signature ring may roll over by the time the
        // paused video is redrawn, and its fence is changed after Present.
        struct B2DrawState {
            REFERENCE_TIME pts = INVALID_TIME;
            uint64_t fence = 0;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            std::array<float, 3> tone{1.0f, 1.0f, 1.0f};
        };
        std::array<B2DrawState, 4> b2DrawRecords{};
        size_t b2DrawSlot = 0;
        B2DrawState b2Presented{};
        // Count *successful Completed Presents*, not speculative/prepared draws.
        uint64_t presentedGraded = 0, presentedUngraded = 0;
		uint64_t skippedCapture = 0, reusedGrade = 0, ungraded = 0;
		uint64_t requests = 0, completed = 0, modelTicks = 0, stageTicks = 0;
		uint64_t temporalFrames = 0, temporalUsed = 0, temporalRejected = 0, temporalFuture = 0, prefetched = 0, earlyPrimed = 0;
		unsigned lastWindow = 0;
		bool attempted = false, failed = false, active = false;
		Tex2D_t thumbnail, staging, picture;
		CComPtr<ID3D11SamplerState> sampler;
		CComPtr<ID3D11Buffer> constants;
		CComPtr<ID3D11PixelShader> shader;
        CComPtr<ID3D11PixelShader> b2LuminanceShader;
        CComPtr<ID3D11PixelShader> b2MemoryShader;
        CComPtr<ID3D11PixelShader> r1GamutShader;
        CComPtr<ID3D11PixelShader> m1SkinShader;
        CComPtr<ID3D11PixelShader> r1M1SkinShader;
	} m_svdLutRawTest;

    // P1/P2 perceptual pipeline state, previously lost when P4 carried an
    // older projection-state include. Preserve the P4 grayProbe above.
    struct {
        CComPtr<ID3D11Buffer> constants;
        CComPtr<ID3D11SamplerState> sampler;
        CComPtr<ID3D11PixelShader> standalone, normalA, rawB;
        bool triedStandalone = false, triedA = false, triedB = false;
        bool failedStandalone = false, failedA = false, failedB = false;
        uint64_t draws = 0, depthUsed = 0, depthFallback = 0;
    } m_perceptual;

	bool m_jvrShaderFailed = false;
	Tex2D_t m_jvrSource;
	Tex2D_t m_jvrFrame;
	CComPtr<ID3D11PixelShader> m_jvrShader;
	CComPtr<ID3D11Buffer> m_jvrConstants;

