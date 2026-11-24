// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CDX11VideoProcessor private state. Keep declaration order; this fragment is not a standalone header.
	friend class CMpcVideoRenderer;
	bool m_bInterpolateAt50pct=true;
	bool m_bPGSSubtitles=false;
	bool SourceIsHDR10() const {return m_decExFmt.VideoTransferFunction==MFVideoTransFunc_2084 && m_decExFmt.VideoTransferMatrix!=DXVA2_VideoTransferMatrix_Unknown;}
	std::atomic<unsigned> m_mfgMultiplier{0};
	mfg::DisplayRateGate m_mfgDisplayRate; // RendererLock
    // One logical AI upscaler, routed to exactly one side of MFG. This
    // selector is read under RendererLock by both the source and presenter.
    enum class UpscaleSlot : unsigned { BeforeMfg = 0, AfterMfg = 1 };
    unsigned m_mfgProcessingMode = 0;
    static constexpr auto MfgOriginalProtection = mfg::OriginalFrameProtection::Prefer;
    unsigned m_mfgOriginalsReplaced = 0; // RendererLock
    bool UpscaleAt(UpscaleSlot slot) const {
        return m_mfgProcessingMode == static_cast<unsigned>(slot);
    }
	// Requested UI multiplier is independent of the display-budgeted effective multiplier.
	std::atomic<unsigned> m_mfgEffectiveMultiplier{0};
	unsigned m_mfgTargetSlots = 0;
	unsigned m_mfgBudgetCandidate = 0;
	unsigned m_mfgBudgetStreak = 0;
	// Auto needs the *actual* interval PTS, including x1 fallback intervals.
	// Kept independent of Session so reducing to x1 does not lose cadence phase.
	int64_t m_mfgAutoEpoch = -1;
	int64_t m_mfgAutoLastPts = -1;
	mfg::RepeatFrameTiming m_mfgRepeatTiming;
	mfg::Session m_mfg;
	bool m_startupHasPicture = false;
	bool m_extendedStartupPending = false;
	mfg::DecodedFrameDifference m_mfgDecodedDifference;
	std::atomic<unsigned> m_mfgPendingRepeats{0};
	bool m_useZipDepthSr = false;
	bool m_useZipDepthMfg = false;
	bool m_adaptiveMfgDepth = false;
	bool m_showMfgMotion = false;
	bool UsesZipDepthSr() const { return m_useZipDepthSr && m_iVPSuperRes == SUPERRES_DLSS_SR; }
	bool MfgSourceMeetsDisplayRate() const;
	bool UsesZipDepthMfg() const { return m_useZipDepthMfg && m_mfgMultiplier && !MfgSourceMeetsDisplayRate(); }
	bool m_showDepthOverlay = false;
	bool m_depth3DPreview = false;
	bool MfgDepthSourceEligible() const;
	bool m_mfgLowMotionEnabled = false;
	bool m_mfgLowMotionActive = false;
	unsigned m_mfgLowMotionStreak = 0;
	unsigned m_mfgQuietFlowCredit = 0;
	int64_t m_mfgQuietFlowPts = -1;
	uint64_t m_mfgLowMotionVetoes = 0;
	uint64_t m_mfgLowMotionSaved = 0;
	bool m_mfgRepeatHold = false;
	uint64_t m_mfgRepeatsBypassed = 0;
	std::shared_ptr<mfg::OriginalSnapshot> m_mfgOriginalPrime;
	void ResetMfgFrameAnalysis();
	bool MfgFrameAnalysisEligible() const;
	std::wstring MfgFrameAnalysisStatus() const;
	mfg::FrameQueue m_mfgQueue; // v7 A/B rollback
	mfg::LiveFlowGroups m_mfgLiveFlowGroups;
	std::mutex m_mfgQueueLock;
	// Adaptive source read-ahead is independent of presentation PTS.
	// Only the source-processing thread trains this estimator.
	std::mutex m_mfgPacingLock;
	mfg::AdaptiveLookahead m_mfgPacing; // v7 A/B rollback
	mfg::LiveFlowGroupPacing m_mfgGroupPacing;
	std::atomic<int64_t> m_mfgExtraAheadTicks{0}; // 100-ns DirectShow ticks
	std::atomic<int64_t> m_mfgReadyLeadUs{0};
	std::atomic<uint64_t> m_mfgReadyLate{0};
    // Presenter-only estimator; StopMfg joins its thread before resetting.
    mfg::DisplaySubmitPacing m_mfgDisplayPacing;
    std::atomic<int64_t> m_mfgDisplayLeadUs{0};
    std::atomic<int64_t> m_mfgDisplayLastLateUs{0};
    std::atomic<int64_t> m_mfgDisplayP95NeedUs{0};
    std::atomic<unsigned> m_mfgDisplaySamples{0};
	std::atomic<uint64_t> m_mfgPresentedLateReal{0},m_mfgPresentedLateGenerated{0};
    // Stage 3: draw video into an independent D3D11 surface ahead of its PTS.
    // The presenter alone owns this state; StopMfg joins the thread before
    // releasing it. The source/NGX bank is consumed after the early GPU draw.
    std::optional<mfg::QueuedFrame> m_mfgPreparedFrame;
    CComPtr<ID3D11Texture2D> m_mfgPreparedTexture;
    D3D11_TEXTURE2D_DESC m_mfgPreparedDesc{};
    bool m_mfgPreparationBroken = false;
    std::atomic<unsigned> m_mfgPreparedOriginalPending{0};
    std::atomic<uint64_t> m_mfgPrepareCount{0}, m_mfgPrepareFallbacks{0};
    std::atomic<int64_t> m_mfgPrepareLeadUs{0};
    std::atomic<int64_t> m_mfgPrepareLastDrawUs{0},m_mfgPrepareReadyMarginUs{0};
	framegen::PresentationThread m_mfgThread;
	// Present counters are written by the MFG presenter thread and read by the
	// regular statistics/COM paths. Keep reads race-free.
	std::atomic<unsigned> m_mfgSkipped{0};
	std::atomic<uint64_t> m_mfgGeneratedPresents{0},m_mfgRealPresents{0},m_mfgFallbackPresents{0};
	std::mutex m_mfgRateLock;
	int64_t m_mfgRateTick=0;
	uint64_t m_mfgRateReal=0,m_mfgRateGenerated=0,m_mfgRateFallback=0;
	unsigned m_mfgRateSkipped=0;
	double m_mfgActualFps=0.0,m_mfgGeneratedFps=0.0,m_mfgSkippedPerSec=0.0,m_mfgFallbackFps=0.0;
	// Present() success counts do not mean the display actually scanned out that image.
	// DXGI GetFrameStatistics is sampled separately, and is explicitly advisory.
	bool m_mfgDxgiHasBaseline=false,m_mfgDxgiAvailable=false;
	unsigned m_mfgDxgiLastPresentCount=0;
	double m_mfgDxgiDisplayFps=0.0;
	// Wall-clock stage timing includes GPU waits; it is NOT GPU execution time.
	std::atomic<uint64_t> m_mfgVideoCount{0},m_mfgVideoTicks{0};
	std::atomic<uint64_t> m_mfgGenerateCount{0},m_mfgGenerateTicks{0};
	std::atomic<uint64_t> m_mfgComposeCount{0},m_mfgComposeTicks{0};
	std::atomic<uint64_t> m_mfgLockRetries{0};
	std::atomic<uint64_t> m_mfgGpuNotReady{0};
	std::atomic<uint64_t> m_mfgDroppedOnAdd{0},m_mfgDroppedDue{0},m_mfgDroppedInvalid{0};
	uint64_t m_mfgRateVideoCount=0,m_mfgRateVideoTicks=0;
	uint64_t m_mfgRateGenerateCount=0,m_mfgRateGenerateTicks=0;
	uint64_t m_mfgRateComposeCount=0,m_mfgRateComposeTicks=0;
	uint64_t m_mfgRateLockRetries=0;
	double m_mfgVideoWallMs=0.0,m_mfgGenerateWallMs=0.0,m_mfgComposeWallMs=0.0,m_mfgLockRetriesPerSec=0.0;
	std::atomic<bool> m_mfgPresentFailed{false};
	// Account for missing frames caused by deadlines and reused resource banks.
	std::atomic<unsigned> m_mfgLostReal{0},m_mfgLostGenerated{0};
	// Optional SDR fast path: source-size RGB after chroma/CSC, before image upscale.
	Tex2D_t m_mfgNativeInput;
	bool m_mfgInputModeKnown=false;
	bool m_mfgLastNativeInput=false;
    bool m_mfgLastPostVsrInput=false;
    bool m_mfgLastVideoOnlyInput = false;
	bool QueueMfg(ID3D11Texture2D* image, REFERENCE_TIME timestamp, bool& overlayInputHandled,
		bool nativeInput, bool postVsrInput, bool videoOnlyInput = false, bool projected = false);
	bool PresentMfg();
	bool NextMfg(int64_t& deadline);
	void StopMfg(bool release=true, bool fastShutdown=false);
    // Settings changes run under RendererLock. Retire the old timeline and
    // present its independent ORIGINAL snapshots before moving the upscaler.
    void SwitchMfgUpscaleSlot(unsigned requestedMode);
    // Coalesce window size changes before resizing the swap chain.
    bool m_resizePending = false;
    CRect m_resizeTarget;
    ULONGLONG m_resizeLastChangeTick = 0;
    ULONGLONG m_completedGeometryTick = 0;
    // Arm B08's original-first pressure escape only for a real window resize.
    // Cleared by ClearCompletedFrames on Seek/Flush/NewSegment.
    std::atomic<bool> m_completedResizeRecovery{false};
    HANDLE m_completedPresentWaitable = nullptr;
    bool m_completedPresentPermit = false;
    HRESULT CreateEditionWindowedSwapChain(DXGI_SWAP_CHAIN_DESC1& desc);
    bool TryCompletedPresentPermit();
    void InvalidateCompletedBackBuffer(bool externalPresent = false);
    UINT EditionSwapChainFlags() const;
    HRESULT ApplyPendingWindowResize(bool settle = true);
    void DiscardMfgPreparedFrame();
    void UpdateProcessingResolution();
    CRect ProcessingRect() const;
    CRect PictureForPlayback(bool projected) const;
    CRect ProcessingSourceRect(bool projected) const;
    CSize m_processingSize{0, 0};
    Tex2D_t m_processingTexture;
    CTex2DRing m_processingPostScale;
    Tex2D_t m_completedResizeTexture;

	HRESULT CompositeMfg(const mfg::FrameSurface& surface,REFERENCE_TIME timestamp,bool nativeInput, uint64_t serial);
    HRESULT RenderMfgVideo(const mfg::FrameSurface& surface, bool nativeInput, ID3D11Texture2D* target,
                           REFERENCE_TIME timestamp, const CRect& targetPicture, bool videoOnlyInput,
                           bool bypassUpscale = false, bool projected = false);
    HRESULT FinishMfgPresent(ID3D11Texture2D* preparedVideo, REFERENCE_TIME timestamp, uint64_t serial,
                             bool completed = false, const CRect* pictureOverride = nullptr,
                             bool svdLutHandled = false, bool alreadyComposited = false);
    void DrawMfgPresentOverlays(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp, uint64_t serial);
    struct CompletedFrame {
        Tex2D_t image;
        CRect picture;
        REFERENCE_TIME pts = 0, end = 0, due = 0;
        uint64_t serial = 0, fence = 0, backBufferReadFence = 0;
        unsigned phase = mfg::MaxGenerated;
        bool mfg = false;
        bool videoOnly = false;
        bool projected = false;
        bool presentRetried = false;
    };
    struct CompletedBatch {
        CompletedFrame target;
        std::vector<mfg::QueuedFrame> phases;
        bool videoOnlyInput = false;
    };
    static constexpr REFERENCE_TIME CompletedLead = 2 * UNITS;
    static constexpr REFERENCE_TIME CompletedStartupLead = UNITS / 4;
    framegen::PresentationThread m_completedPresenter;
    bool m_fastShutdown = false;
    mfg::LiveFlowGroups m_completedGroups;
    mfg::DisplaySubmitPacing m_completedDisplayPacing;
    std::deque<CompletedFrame> m_completedFrames;
    struct CompletedBackBuffer {
        uint64_t frameFence = 0;
        CRect picture = CRect(0, 0, 0, 0);
        bool drawn = false;
        // Actual grade state of the cached pre-draw, not a later attempt.
        bool graded = false;
        // DXGI_PRESENT_DO_NOT_WAIT can reject a fully composed back buffer.
        // These are non-owning identity fields: never retain a COM reference
        // across ResizeBuffers. InvalidateCompletedBackBuffer clears them.
        bool busyRetryReady = false;
        ID3D11Texture2D* busyRetryBuffer = nullptr;
    };
    CompletedBackBuffer m_completedBackBuffer;
    std::vector<CompletedFrame> m_completedFree;
    std::optional<CompletedFrame> m_completedLast;
    std::optional<CompletedFrame> m_completedTarget;
    std::deque<CompletedBatch> m_completedBatches;
    CComPtr<ID3D11DeviceContext4> m_completedContext;
    CComPtr<ID3D11Fence> m_completedFence;
    CComPtr<IDXGIAdapter3> m_completedAdapter;
    uint64_t m_completedFenceValue = 0, m_completedByteLimit = 0;
    ULONGLONG m_completedBudgetTick = 0;
    ULONGLONG m_completedBufferStatsTick = 0;
	bool m_showEditionDetails = false;
	unsigned m_editionStatisticsRun = 0;
	unsigned m_editionOriginalDropsBase = 0;
	std::optional<unsigned> m_completedBufferPercent;
	edition::BufferLowWatermark m_completedBufferLow;
	edition::PreDrawWindow m_completedPreDraw;
	struct VsrStatistics {
		CSize input{0, 0}, output{0, 0};
		ULONGLONG tick = 0;
		bool afterMfg = false;
	} m_editionVsr;
	void RecordEditionVsr(const CRect& input, const CRect& output, bool afterMfg);
    std::wstring m_completedBufferStats;
    struct CompletedStatsDrops {
        unsigned ngxLate = 0, composeLate = 0, presentLate = 0, budget = 0, other = 0;
    } m_completedStatsDrops;
    struct CompletedCadence {
        struct Range {
            int64_t low = INT64_MAX, high = INT64_MIN;
            void Add(int64_t value) {
                low = (std::min)(low, value);
                high = (std::max)(high, value);
            }
        };
        unsigned playbackRun = 0;
        uint64_t lastTick = 0;
        REFERENCE_TIME lastPts = 0, lastDue = 0;
        Range submit, media, due;
        void Observe(unsigned run, uint64_t tick, REFERENCE_TIME pts, REFERENCE_TIME deadline) {
            if (playbackRun != run) *this = {};
            if (lastTick && tick > lastTick) {
                submit.Add(int64_t(tick - lastTick));
                media.Add(pts - lastPts);
                due.Add(deadline - lastDue);
            }
            playbackRun = run;
            lastTick = tick;
            lastPts = pts;
            lastDue = deadline;
        }
        void ClearWindow() { submit = {}; media = {}; due = {}; }
    } m_completedCadence;
    DXGI_QUERY_VIDEO_MEMORY_INFO m_completedMemoryInfo{};
    bool m_completedBudgetKnown = false;
    bool m_completingSample = false, m_completedFailed = false;
    bool CanBufferCompletedSample() const;
    bool CompletedFramesPending() const;
    bool LegacyFramesPending();
    const char* CompletedAdmissionBlock(REFERENCE_TIME due, std::optional<uint64_t>& deferredFrameFence);
    const char* CompletedOverlapBlock(REFERENCE_TIME stream, REFERENCE_TIME margin, uint64_t completed,
                                      uint64_t committed, uint64_t nextFrameBytes) const;
    struct CompletedPoolUsage {
        uint64_t active = 0;
        uint64_t gpuPending = 0;
        uint64_t reusable = 0;
        uint64_t Committed() const { return active + gpuPending; }
        uint64_t Retained() const { return Committed() + reusable; }
    };
    CompletedPoolUsage GetCompletedPoolUsage(uint64_t completed) const;
    uint64_t PendingGeneratedBytes() const;
    void RefreshCompletedMemoryBudget(uint64_t completed);
    HRESULT RenderCompletedSample(IMediaSample* sample);
    HRESULT PrepareCompletedTarget();
    HRESULT AcquireCompletedTarget(const D3D11_TEXTURE2D_DESC& desc, CompletedFrame& target);
    void RecycleCompletedFrame(CompletedFrame&& frame);
    bool QueueCompletedFrame(CompletedFrame&& frame);
    bool PrepareCompletedBatch(REFERENCE_TIME stream, bool originalPriority = false);
    bool NextCompletedFrame(int64_t& deadline);
    bool PresentCompletedFrame();
    REFERENCE_TIME CompletedSubmitLead();
    REFERENCE_TIME CompletedBackBufferLead(const CompletedFrame& frame, REFERENCE_TIME submitLead) const;
    bool PrepareCompletedBackBuffer(CompletedFrame& frame);
    bool CompletedGeometryChanging() const;
    HRESULT DrawCompletedBackBuffer(const CompletedFrame& frame, ID3D11Texture2D* target,
                                    const CRect& picture, bool geometryChanging, bool scheduled);
    HRESULT DrawCompletedVideo(const CompletedFrame& frame, ID3D11Texture2D* target, const CRect& picture,
                               bool geometryChanging = false, const CRect* targetBounds = nullptr);
	const Tex2D_t* PrepareDepth3DPreview(const CompletedFrame& frame, const CRect& sourceRect);
	bool EnsureDepth3DMeshResources();
    HRESULT PresentCompletedVideo(const CompletedFrame& frame, bool scheduled = false, uint64_t* preparedReadFence = nullptr);
    void ClearCompletedFrames(bool release);
    struct CompletedBufferTime {
        REFERENCE_TIME ready = 0;
        REFERENCE_TIME queued = 0;
        size_t readyFrames = 0;
        size_t queuedFrames = 0;
        uint64_t fence = UINT64_MAX;
        unsigned nonIncreasing = 0;
    };
    CompletedBufferTime GetCompletedBufferTime(REFERENCE_TIME stream) const;
    std::wstring CompletedBufferInfo();
    void DrawMfgMotion(ID3D11Texture2D* target, uint64_t serial, REFERENCE_TIME pts, const CRect& picture);
    HRESULT EnsureMfgPreparedTexture(ID3D11Texture2D* referenceBackBuffer);
    bool MfgPostUpscaleRequested() const;
    bool EnsureMfgPostVsr();
    void ResetMfgPostVsr();

    std::atomic<bool> m_showFeatureStatus{true};
    std::mutex m_featureStatusMutex;
    uint64_t m_featureStatusLastGenerated = 0;
    ULONGLONG m_featureStatusGeneratedTick = 0;
    ULONGLONG m_featureStatusUntil = 0;
    CSize m_featureStatusSize{0, 0};
    unsigned m_featureStatusPlaybackRun = 0;
    bool m_featureStatusMfgActive = false;
    void ResetFeatureStatus();
    LONG DrawFeatureStatus(ID3D11Texture2D* target);
    LONG m_statsFeatureOffset = 0;
    // Rate-limit Statistics text formatting; drawing stays in the existing OSD pass.
    std::wstring m_cachedStatisticsText;
    ULONGLONG m_cachedStatisticsTick = 0;
    unsigned m_cachedStatisticsRun = 0;
    // Owned by this renderer, never a thread with static-DLL lifetime.
    std::unique_ptr<edition::GpuLoadSampler> m_gpuLoadSampler;

