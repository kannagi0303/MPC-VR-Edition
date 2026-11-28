// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CDX11VideoProcessor projection/color state. Keep declaration order; this fragment is not a standalone header.
	bool m_jvrFlatView = false;
	int m_jvrEye = 0;
	jvr::ViewTransform m_jvrView;
	std::array<float, 16> m_jvrUploadedConstants{};
    // Shared standalone colour fallback surface; no retired A model worker.
    struct {
        bool clearViewSupported = false; // preserve existing Temporal B clear policy
        Tex2D_t picture;
    } m_svdLut;
	// Shared FP32 preprocessing shader.
	CComPtr<ID3D11PixelShader> m_svdLutThumbnailShader;
	CComPtr<ID3D11Buffer> m_svdLutThumbnailConstants;

    // Original input identity (not a LUT arrival time or estimated FPS).
    // No ownership or scheduling changes. Duplicate PTS is not a new frame.
    struct SvdLutSourceTimeline {
        REFERENCE_TIME current = INVALID_TIME, previous = INVALID_TIME;
        void Observe(REFERENCE_TIME pts) noexcept {
            if (pts < 0) { current = previous = INVALID_TIME; return; }
            if (pts == current) return;
            previous = current >= 0 && current < pts ? current : INVALID_TIME;
            current = pts;
        }
        REFERENCE_TIME PreviousFor(REFERENCE_TIME pts) const noexcept {
            return current == pts ? previous : INVALID_TIME;
        }
    };
	// Video Enhancement temporal analysis state (former B).
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
        // FIX12: conservative PTS span of raw inputs contributing to temporalView.
        std::array<REFERENCE_TIME, 2> temporalInputPts{INVALID_TIME, INVALID_TIME};
        svdlut::temporal::Contributions temporalContributions{};
	};
	// Video Enhancement uses these asynchronous readback slots. Map(DO_NOT_WAIT)
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
        SvdLutSourceTimeline sourceTimeline;
		std::array<SvdLutRawFrame, 16> frames;
		std::array<SvdLutTemporalCapture, 3> captures;
		size_t frameSlot = 0, captureSlot = 0;
		bool inFlight = false;
		REFERENCE_TIME inFlightPts = INVALID_TIME;
		uint64_t inFlightFence = 0;
        // Keep the latest completed LUT independent of the 16-frame ring.
        // Otherwise a slow result can be lost when its source frame is evicted.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> heldView;
        std::shared_ptr<const svdlut::Parameters> heldParameters;
        std::array<float, 8 * 8 * 3> heldSignature{}, inFlightSignature{};
        REFERENCE_TIME heldPts = INVALID_TIME;
        uint64_t heldFence = 0;
        std::array<REFERENCE_TIME, 2> heldInputPts{INVALID_TIME, INVALID_TIME};
        svdlut::temporal::Contributions heldContributions{};
        bool heldSignatureReady = false;
        // Producer-side attempts, not unique frames or DXGI Presents.
        uint64_t sourceLutChecks = 0, sourceLutExactReady = 0;
        uint64_t sourceLutDraws = 0, sourceLutFutureDraws = 0;
        double sourceLutFutureShareSum = 0.0;
        // B2.1 is a fixed, strictly post-B1 adaptive luma correction.
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
        uint64_t b2Analyses = 0;
        uint64_t b2AnalysisTicks = 0, b2BudgetSkips = 0;
        bool b2ShaderAttempted = false, b2ShaderFailed = false;
        bool b2MemoryShaderAttempted = false, b2MemoryShaderFailed = false;
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
		uint64_t skippedCapture = 0;
		uint64_t completed = 0; // also used by optional-capture admission
		bool attempted = false, failed = false, active = false;
        REFERENCE_TIME activationPts = INVALID_TIME; // VE-08: first successfully presented grade
		Tex2D_t thumbnail, picture;
		CComPtr<ID3D11SamplerState> sampler;
		CComPtr<ID3D11Buffer> constants;
        CComPtr<ID3D11PixelShader> b2LuminanceShader;
        CComPtr<ID3D11PixelShader> b2MemoryShader;
	} m_svdLutRawTest;

    // [AT-03 | 2026-11-25 11:25 +08:00] Independent SDR tone state, RendererLock only.
    struct AdaptiveToneShaderJob { CComPtr<ID3DBlob> code, sampleCode; std::atomic_bool sampleReady=false, ready=false; };
    struct {
        edition::adaptive_tone::Shot shot;
        Tex2D_t thumbnail, staging;
        CComPtr<ID3D11Buffer> curve, constants, sampleConstants;
        CComPtr<ID3D11PixelShader> shader, sampleShader;
        std::shared_ptr<AdaptiveToneShaderJob> job;
        REFERENCE_TIME requestPts=INVALID_TIME, capturePts=INVALID_TIME, uploadedPts=INVALID_TIME;
        ULONGLONG primeTick=0;
        bool pending=false, capturePrimed=false, suspended=false, captureAfterSuspend=false;
        // Early observations share the existing single readback. Until the
        // original RGB is actually consumed they cannot update the shot.
        bool captureEarly=false, captureSpeculative=false;
        uint64_t captureEpoch=0;
        REFERENCE_TIME previousRequestPts=INVALID_TIME;
        bool shaderFailed=false, allowDraw=false, applied=false;
        bool sourceComposition=false; // FA-03A: scoped to the producer draw
        Tex2D_t sourceOutput;
        // Native RGB original, plus the existing-order VSR bridge. Reused and
        // retired with optional colour resources; no presenter/bank ownership.
        Tex2D_t sourceRaw, sourceVsrOutput;
        CD3D11VP sourceCscVP, sourceVsrVP;
        std::array<UINT, 7> sourceVsrKey{}; // includes the full source colour metadata
        bool sourceVsrReady=false;
        // FA-03B: render-local upstream decision; not a persistent effect flag.
        bool sourceToneBeforeUpscale=false;
        std::optional<bool> srSourceToneRoute;
    } m_adaptiveTone;

    // FIX16: raw RGB for the *currently arriving* original, prepared before
    // Completed admission only in supported native SDR CSC / original-first SR routes.
    // No decoder sample ownership: identity is borrowed within Receive.
    struct EarlyOriginalImage {
        Tex2D_t rgb;
        CRect rect;
        // The converter can be reinitialized while a different slot still
        // owns its output. The image must carry its own colour-domain key.
        std::array<UINT, 5> sourceKey{};
        const IMediaSample* sample = nullptr;
        const ID3D11Texture2D* decoderTexture = nullptr;
        UINT decoderSlice = 0;
        REFERENCE_TIME pts = INVALID_TIME;
        uint64_t epoch = 0;
        // The image travels between current/future slots, including its scratch owner.
        bool srSource = false;
        bool ready = false, approved = false, consumed = false, captured = false;
    };
    struct EarlyOriginalState : EarlyOriginalImage {
        CD3D11VP vp;
        CComPtr<ID3D11Query> completion;
        std::array<UINT, 5> key{}; // converter configuration, not image identity
        bool queryIssued = false, broken = false;
    } m_earlyOriginal;
    // At most one actual future original. Its sample is owned by the input
    // queue; this slot only owns its converted RGB and borrowed identity.
    EarlyOriginalImage m_futureOriginal;

    // P1/P2 perceptual pipeline state, previously lost when P4 carried an
    // older projection-state include.
    struct {
        CComPtr<ID3D11Buffer> constants;
        CComPtr<ID3D11SamplerState> sampler;
        CComPtr<ID3D11PixelShader> standalone, rawB;
        bool triedStandalone = false, triedB = false;
        bool failedStandalone = false, failedB = false;
        uint64_t draws = 0, depthUsed = 0, depthFallback = 0;
    } m_perceptual;

    // Dim-only temporal field; never aliases RAW depth or the AI input.
    struct {
        Tex2D_t masks[2], alignment;
        CComPtr<ID3D11PixelShader> maskShader, alignmentShader;
        CComPtr<ID3D11Buffer> constants;
        REFERENCE_TIME pts = INVALID_TIME;
        uint64_t generation = 0;
        UINT sourceWidth = 0, sourceHeight = 0;
        bool rotated = false, projected = false, shaderFailed = false;
        std::array<float,16> projectionKey{};
        unsigned history = 0;
    } m_farDimmingField;

	bool m_jvrShaderFailed = false;
	Tex2D_t m_jvrSource;
	Tex2D_t m_jvrFrame;
	CComPtr<ID3D11PixelShader> m_jvrShader;
	CComPtr<ID3D11Buffer> m_jvrConstants;

