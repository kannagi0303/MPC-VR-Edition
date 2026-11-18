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

#include <DXGI1_2.h>
#include <dxva2api.h>
#include <dxgi1_5.h>
#include <strmif.h>
#include <format>
#include <map>
#include "IVideoRenderer.h"
#include "DX11Helper.h"
#include "JvrProjection.h"
#include "D3D11VP.h"
#include "D3DUtil/D3D11Font.h"
#include "D3DUtil/D3D11Geometry.h"
#include "VideoProcessor.h"
#include "SubPic/DX11SubPic.h"

#include <array>
#include <atomic>
#include <mutex>
#include "Mfg/Session.h"
#include "DLSS/DlssSR.h"
#include "DLSS/ZipDepth.h"
#include "DLSS/SvdLutModel.h"
#include "Mfg/FrameQueue.h"
#include "Mfg/LiveFlowGroups.h"
#include "Mfg/LiveFlowGroupPacing.h"
#include "Mfg/AdaptiveLookahead.h"
#include "Mfg/DisplayCadence.h"
#include "Mfg/DisplaySubmitPacing.h"
#include "Mfg/DisplayPreparation.h"
#include "Mfg/AutoCadence.h"
#include "Mfg/RepeatFrameTiming.h"
#include "Mfg/DecodedFrameDifference.h"
#include "Mfg/PresentationThread.h"


class CVideoRendererInputPin;

class CDX11VideoProcessor
	: public CVideoProcessor
{
private:
	friend class CVideoRendererInputPin;
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
                             bool svdLutHandled = false);
    struct CompletedFrame {
        Tex2D_t image;
        CRect picture;
        REFERENCE_TIME pts = 0, end = 0, due = 0;
        uint64_t serial = 0, fence = 0;
        unsigned phase = mfg::MaxGenerated;
        bool mfg = false;
        bool videoOnly = false;
        bool projected = false;
    };
    struct CompletedBatch {
        CompletedFrame target;
        std::vector<mfg::QueuedFrame> phases;
        bool videoOnlyInput = false;
    };
    static constexpr REFERENCE_TIME CompletedLead = 2 * UNITS;
    framegen::PresentationThread m_completedPresenter;
    bool m_fastShutdown = false;
    mfg::LiveFlowGroups m_completedGroups;
    mfg::DisplaySubmitPacing m_completedDisplayPacing;
    std::deque<CompletedFrame> m_completedFrames;
    std::vector<CompletedFrame> m_completedFree;
    std::optional<CompletedFrame> m_completedLast;
    std::optional<CompletedFrame> m_completedTarget;
    std::deque<CompletedBatch> m_completedBatches;
    CComPtr<ID3D11DeviceContext4> m_completedContext;
    CComPtr<ID3D11Fence> m_completedFence;
    CComPtr<IDXGIAdapter3> m_completedAdapter;
    uint64_t m_completedFenceValue = 0, m_completedByteLimit = 0;
    ULONGLONG m_completedBudgetTick = 0;
    bool m_completingSample = false, m_completedFailed = false;
    bool CanBufferCompletedSample() const;
    bool CompletedFramesPending() const;
    bool LegacyFramesPending();
    bool CanProduceCompletedFrame(REFERENCE_TIME due, std::optional<uint64_t>& deferredFrameFence);
    HRESULT RenderCompletedSample(IMediaSample* sample);
    bool PrepareCompletedTarget();
    bool AcquireCompletedTarget(const D3D11_TEXTURE2D_DESC& desc, CompletedFrame& target);
    void RecycleCompletedFrame(CompletedFrame&& frame);
    bool QueueCompletedFrame(CompletedFrame&& frame);
    bool PrepareCompletedBatch(REFERENCE_TIME stream);
    bool NextCompletedFrame(int64_t& deadline);
    bool PresentCompletedFrame();
    REFERENCE_TIME CompletedSubmitLead();
    bool CompletedGeometryChanging() const;
    HRESULT DrawCompletedVideo(const CompletedFrame& frame, ID3D11Texture2D* target, const CRect& picture,
                               bool geometryChanging = false, const CRect* targetBounds = nullptr);
	const Tex2D_t* PrepareDepth3DPreview(const CompletedFrame& frame, const CRect& sourceRect);
	bool EnsureDepth3DMeshResources();
    HRESULT PresentCompletedVideo(const CompletedFrame& frame, bool scheduled = false);
    void ClearCompletedFrames(bool release);
    struct CompletedBufferTime {
        REFERENCE_TIME ready = 0;
        REFERENCE_TIME queued = 0;
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


	// Direct3D 11
	CComPtr<ID3D11Device1>        m_pDevice;
	CComPtr<ID3D11DeviceContext1> m_pDeviceContext;
	CComPtr<ID3D11SamplerState>   m_pSamplerPoint;
	CComPtr<ID3D11SamplerState>   m_pSamplerLinear;
	CComPtr<ID3D11SamplerState>   m_pSamplerDither;
	CComPtr<ID3D11BlendState>     m_pAlphaBlendState;
	CComPtr<ID3D11VertexShader>   m_pVS_Simple;
	CComPtr<ID3D11PixelShader>    m_pPS_Simple;
	CComPtr<ID3D11PixelShader>    m_pPS_BitmapToFrame;
	CComPtr<ID3D11PixelShader>    m_pPS_DepthOverlay;
	CComPtr<ID3D11BlendState>     m_pDepthOverlayBlendState;
	CComPtr<ID3D11Buffer>         m_pDepthOverlayConstants;
	Tex2D_t m_depthOverlayCapture;
	CComPtr<ID3D11VertexShader>   m_pVS_Depth3DPreview;
	CComPtr<ID3D11PixelShader>    m_pPS_Depth3DPreview;
	CComPtr<ID3D11InputLayout>    m_pVDepth3DPreviewInputLayout;
	CComPtr<ID3D11Buffer>         m_pDepth3DPreviewConstants;
	CComPtr<ID3D11Buffer>         m_pDepth3DPreviewVertexBuffer;
	CComPtr<ID3D11Buffer>         m_pDepth3DPreviewIndexBuffer;
	CComPtr<ID3D11RasterizerState> m_pDepth3DPreviewRasterizer;
	UINT                         m_depth3DPreviewIndexCount = 0;
	Tex2D_t m_depth3DPreviewTexture;
	bool m_depth3DPreviewShaderFailed = false;
	CComPtr<ID3D11PixelShader> m_pMfgMotionShader;
	CComPtr<ID3D11BlendState> m_pMfgMotionBlend;
	CComPtr<ID3D11Buffer> m_pMfgMotionConstants;
	bool m_depthOverlayShaderFailed = false;
	CComPtr<ID3D11InputLayout>    m_pVSimpleInputLayout;
	CComPtr<ID3D11Buffer>         m_pVertexBuffer;
	CComPtr<ID3D11Buffer>         m_pResizeShaderConstantBuffer;
	CComPtr<ID3D11Buffer>         m_pHalfOUtoInterlaceConstantBuffer;
	CComPtr<ID3D11Buffer>         m_pFinalPassConstantBuffer;

	DXGI_SWAP_EFFECT              m_UsedSwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	Tex11Video_t m_TexSrcVideo; // for copy of frame
	Tex2D_t m_TexConvertOutput;
	Tex2D_t m_TexResize;        // for intermediate result of two-pass resize
	CTex2DRing m_TexsPostScale;
	Tex2D_t m_TexDither;
    CD3D11Rectangle m_FeatureStatusBackground;

	// for GetAlignmentSize()
	struct Alignment_t {
		Tex11Video_t texture;
		ColorFormat_t cformat = {};
		LONG cx = {};
	} m_Alignment;

	// D3D11 Video Processor
	CD3D11VP m_D3D11VP;
    // Opt-in Hybrid: decoded YUV -> separate no-VSR VP -> NGX MFG at source
    // size -> separate BGRA-input RTX VSR VP -> D3D11 Present. Do not modify
    // the existing decoder VP, including its NVIDIA VSR driver state.
    CD3D11VP m_mfgPreVsrVP;
    CD3D11VP m_mfgPostVsrVP;
    Tex2D_t m_mfgPostVsrBGRA; // RGBA NGX output normalized for the single VP
    UINT m_mfgPostVsrWidth = 0, m_mfgPostVsrHeight = 0;
    DXGI_FORMAT m_mfgPostVsrSourceFmt = DXGI_FORMAT_UNKNOWN;
    int m_mfgPostVsrSetting = SUPERRES_Disable;
    bool m_mfgPostVsrReady = false, m_mfgPostVsrBroken = false;
    std::atomic<uint64_t> m_mfgPostVsrProcessed{0}, m_mfgPostVsrFallback{0};

	CComPtr<ID3D11Buffer> m_pCorrectionConstants;
	CComPtr<ID3D11PixelShader> m_pPSCorrection;

	// per-frame measurement of the content peak, for the HDR to SDR conversion and local tone mapping
	CComPtr<ID3D11ComputeShader>        m_pCSHdrHist;
	CComPtr<ID3D11ComputeShader>        m_pCSHdrResolve;
	CComPtr<ID3D11Buffer>               m_pHdrHistBuffer;
	CComPtr<ID3D11UnorderedAccessView>  m_pHdrHistUAV;
	CComPtr<ID3D11Buffer>               m_pHdrStateBuffer;
	CComPtr<ID3D11UnorderedAccessView>  m_pHdrStateUAV;
	CComPtr<ID3D11ShaderResourceView>   m_pHdrStateSRV;
	CComPtr<ID3D11Buffer>               m_pHdrMeasureConstants;
	CComPtr<ID3D11Buffer>               m_pHdrResolveConstants;
	CComPtr<ID3D11Buffer>               m_pHdrStateStaging; // for the statistics only
	bool  m_bSdrMeasureActive     = false; // the measuring variant of the correction shader is in use
	bool  m_bHdrLocalMeasureActive = false; // the measuring variant of the local tone mapping shader is in use
	bool  m_bHdrStatsCopyPending  = false;
	float m_fHdrMeasuredPeakNits  = 0.0f;
	float m_fHdrSmoothedPeakNits  = 0.0f;

	struct HdrMeasureConstants_t {
		UINT rectOrigin[2];
		UINT rectSize[2];
	};
	struct HdrResolveConstants_t { // must match cbuffer ResolveConstants in cs_hdr_resolve.hlsl
		float frameTime;
		float releaseTime;
		float sceneCutPQ;
		float peakFraction;
		float windowTime;
		float peakFloor;
		float padding[2];
	};
	const wchar_t* m_strCorrection = nullptr;

	// HDR tonemapping
	struct HDRParamsConstantBuffer_t {
		float MasteringMinLuminanceNits;
		float MasteringMaxLuminanceNits;
		float maxCLL;
		float maxFALL;
		float displayMaxNits;
		UINT selection; // 1 = ACES, 2 = Reinhard, 3 = Habel, 4 = Möbius, 5 = BT2390, 6 = ST 2094-10
		float padding[2];
	};
	struct DoViDynamicConstantsBuffer_t {
		float trim_chroma_weight;
		float trim_saturation_gain;
		float trim_slope;
		float trim_offset;
		float trim_power;
		UINT enabled;
		float padding[2];
	};
	HDRParamsConstantBuffer_t m_lastHDRParamsConstantBuffer = {};
	DoViDynamicConstantsBuffer_t m_lastDoViDynamicConstantsBuffer = {};
	CComPtr<ID3D11Buffer> m_pHDR10ToneMappingConstants;
	CComPtr<ID3D11Buffer> m_pDoViDynamicConstants;
	CComPtr<ID3D11PixelShader> m_pPSHDR10ToneMapping;

	// D3D11 Shader Video Processor
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

	bool m_jvrShaderFailed = false;
	Tex2D_t m_jvrSource;
	Tex2D_t m_jvrFrame;
	CComPtr<ID3D11PixelShader> m_jvrShader;
	CComPtr<ID3D11Buffer> m_jvrConstants;

	CComPtr<ID3D11PixelShader> m_pPSConvertColor;
	CComPtr<ID3D11PixelShader> m_pPSConvertColorDeint;
	struct {
		bool bEnable = false;
		ID3D11Buffer* pVertexBuffer = nullptr;
		ID3D11Buffer* pConstants = nullptr;
		void Release() {
			bEnable = false;
			SAFE_RELEASE(pVertexBuffer);
			SAFE_RELEASE(pConstants);
		}
	} m_PSConvColorData;

	CComPtr<ID3D11Buffer> m_pDoviCurvesConstantBuffer;

	CComPtr<ID3D11PixelShader> m_pShaderUpscaleX;
	CComPtr<ID3D11PixelShader> m_pShaderUpscaleY;
	CComPtr<ID3D11PixelShader> m_pShaderDownscaleX;
	CComPtr<ID3D11PixelShader> m_pShaderDownscaleY;

	std::vector<ExternalPixelShader11_t> m_pPreScaleShaders;
	std::vector<ExternalPixelShader11_t> m_pPostScaleShaders;
	CComPtr<ID3D11Buffer> m_pPostScaleConstants;
	CComPtr<ID3D11PixelShader> m_pPSHalfOUtoInterlace;
	CComPtr<ID3D11PixelShader> m_pPSFinalPass;

	CComPtr<IDXGIFactory2>   m_pDXGIFactory2;
	CComPtr<IDXGISwapChain1> m_pDXGISwapChain1;
	CComPtr<IDXGISwapChain4> m_pDXGISwapChain4;
	CComPtr<IDXGIOutput>    m_pDXGIOutput;
	DXGI_COLOR_SPACE_TYPE m_currentSwapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

	// Input parameters
	DXGI_FORMAT m_srcDXGIFormat = DXGI_FORMAT_UNKNOWN;

	// D3D11 VP texture format
	DXGI_FORMAT m_D3D11OutputFmt = DXGI_FORMAT_UNKNOWN;

	// intermediate texture format
	DXGI_FORMAT m_InternalTexFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

	// swap chain format
	DXGI_FORMAT m_SwapChainFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
	UINT32 m_DisplayBitsPerChannel = 8;

	D3D11_VIDEO_FRAME_FORMAT m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;

	CComPtr<IDXGIFactory1> m_pDXGIFactory1;

	bool m_bSubPicWasRendered = false;

	// AlphaBitmap
	Tex2D_t m_TexAlphaBitmap;
	CComPtr<ID3D11Buffer> m_pAlphaBitmapVertex;

	// Statistics
	CD3D11Rectangle m_StatsBackground;
	CD3D11Font      m_Font3D;
	CD3D11Rectangle m_Rect3D;
	CD3D11Rectangle m_Underlay;
	CD3D11Lines     m_Lines;
	CD3D11Polyline  m_SyncLine;

	bool m_bExclusiveScreen = false;
	bool m_bFullScreen = false;

	int m_iVPSuperRes = SUPERRES_Disable;
    int m_dlssSRSourceLimit = DLSSSR_SOURCE_Disable;
	bool m_dlssSRDlaa = false;
	CDlssSR m_dlssSR;
	CZipDepth m_zipDepth;
	CComPtr<CMemAllocator> m_lookaheadAllocator;
	const wchar_t* m_dlssSRInputStatus = nullptr;
	bool m_dlssSRAfterMfg = false;
	Tex2D_t m_dlssSRSource;
	bool m_bVPUseSuperRes = false; // but it is not exactly

	bool m_bVPRTXVideoHDR = false;
	bool m_bVPUseRTXVideoHDR = false;

	bool m_bHdrPassthroughSupport             = false;
	std::atomic_bool m_bHdrDisplaySwitching   = false; // switching HDR display in progress
	bool m_bHdrDisplayModeEnabled             = false;
	bool m_bHdrAllowSwitchDisplay             = true;
	bool m_bACMEnabled                        = false;

	UINT m_srcVideoTransferFunction = 0; // need a description or rename

	std::map<std::wstring, bool> m_hdrModeSavedState;
	std::map<std::wstring, bool, std::less<>> m_hdrModeStartState;

	struct HDRMetadata {
		DXGI_HDR_METADATA_HDR10 hdr10 = {};
		bool bValid = false;
	};
	HDRMetadata m_hdr10 = {};
	HDRMetadata m_lastHdr10 = {};

	UINT m_DoviMaxMasteringLuminance = 0;
	UINT m_DoviMinMasteringLuminance = 0;
	UINT m_DoviMaxContentLightLevel = 0;
	UINT m_DoviMaxFrameAverageLightLevel = 0;

	struct DoviExtensionMetadata_t {
		struct L1_t {
			uint16_t min_pq = 0;
			uint16_t max_pq = 0;
			uint16_t avg_pq = 0;
			bool present = false;

			bool operator==(const L1_t& other) const {
				return min_pq == other.min_pq && max_pq == other.max_pq && avg_pq == other.avg_pq;
			}
			bool operator!=(const L1_t& other) const {
				return !(*this == other);
			}
		};
		L1_t L1;
		L1_t L1Cached;

		struct L2_t {
			float trim_slope = 0.f;
			float trim_offset = 0.f;
			float trim_power = 0.f;
			float trim_saturation_gain = 0.f;
			float trim_chroma_weight = 0.f;
			bool present = false;
		};
		L2_t L2;
	} m_DoviExtensionMetadata;

	HMONITOR m_lastFullscreenHMonitor = nullptr;

	D3DCOLOR m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);

	// SubPic
	CComPtr<CDX11SubPicAllocator> m_pSubPicAllocator;
	bool m_bCallbackDeviceIsSet = false;
	void SetCallbackDevice();
	void UpdateSubPic();

	std::atomic_bool m_bDisplayModeChangeAfterHDRToggle = false;
	CCritSec m_HDRToggleLock;

	bool m_bHDRModeChangeOutside = false;

	void FillDisplayParams();

public:
	CDX11VideoProcessor(CMpcVideoRenderer* pFilter, const Settings_t& config, HRESULT& hr);
	~CDX11VideoProcessor() override;

	int Type() override { return VP_DX11; }
	int GetRenderAhead() override;
	CComPtr<IMediaSample> CloneLookaheadSample(IMediaSample* sample, int64_t& pts, uint64_t& bytes);
	bool PrefetchZipDepth(IMediaSample* sample, int64_t pts);
	void StopZipDepthLookahead() { m_zipDepth.StopLookahead(); }
	unsigned GetVideoProcessorUse() override { return m_D3D11VP.IsReady() ? 1u : 0u; }
	std::wstring GetMfgStatus() override {
		const unsigned multiplier = m_mfgMultiplier.load();
		if (!multiplier) return L"Processor MFG: Off (NGX is not called)";
		if (MfgSourceMeetsDisplayRate()) return L"Processor MFG: Bypassed (source meets display refresh)";
		if (m_mfgPresentFailed.load()) return L"Processor MFG: " + (multiplier==mfg::AutoMultiplier ? L"Auto" : L"x"+std::to_wstring(multiplier)) +
			L"; presenter failed, normal playback retained";
		std::lock_guard guard(m_mfgRateLock);
		std::wstring diagnostics = std::format(
			L"\nMFG runtime    : cap x{}, needs x{}; fallback {:.1f}/s, skipped {:.1f}/s",
			m_mfg.MaxAvailableMultiplier(), m_mfgTargetSlots, m_mfgFallbackFps, m_mfgSkippedPerSec);
		if (UsesZipDepthMfg()) {
			diagnostics += std::format(L"\nMFG depth      : {}", m_mfg.UsesZipDepth() ? L"ZipDepth active" :
				!MfgDepthSourceEligible() ? L"flat (unsupported source)" : m_zipDepth.Status());
		}
		const std::wstring& ngx = m_mfg.Status();
		return L"Processor MFG: " + (multiplier==mfg::AutoMultiplier ? L"Auto" : L"x"+std::to_wstring(multiplier)) + L"; " +
			(ngx.empty() ? L"NGX not yet attempted (waiting for eligible running frame)" : ngx) +
			L"; presents generated=" + std::to_wstring(m_mfgGeneratedPresents.load()) +
			L" real=" + std::to_wstring(m_mfgRealPresents.load()) +
			L" skipped=" + std::to_wstring(m_mfgSkipped.load()) +
			L"; original protection " + mfg::OriginalFrameProtectionName(MfgOriginalProtection) +
			L", replaced=" + std::to_wstring(m_mfgOriginalsReplaced) + diagnostics + MfgFrameAnalysisStatus();
	}

	HRESULT Init(const HWND hwnd, const bool displayHdrChanged, bool* pChangeDevice = nullptr) override;
	bool Initialized() const;

private:
	void ReleaseVP();
	void ReleaseDevice();
	void ReleaseSwapChain();

	UINT GetPostScaleSteps();

	HRESULT CreatePShaderFromResource(ID3D11PixelShader** ppPixelShader, UINT resid);
	void SetShaderConvertColorParams();
	float GetSdrToneMappingPeak() const;
	bool HdrMeasureSupported() const;
	HRESULT InitHdrMeasure();
	void ReleaseHdrMeasure();
	HRESULT MeasureHdrPeak(const Tex2D_t& tex, const CRect& rect);
	void ReadHdrMeasureStats();
	void SetShaderLuminanceParams();

	int HdrLocalToneMappingType() const;
	HRESULT CreateHDR10ToneMappingShader();
	void SetHDR10ShaderParams(float, float, float, float, float, int);
	void SetDolbyVisionDynamicParams();

	HRESULT SetShaderDoviCurvesPoly();
	HRESULT SetShaderDoviCurves();

	void UpdateTexParams(int cdepth);
	void UpdateRenderRect();
	void UpdateScalingStrings();

	void CalcStatsParams() override;

	HRESULT MemCopyToTexSrcVideo(const BYTE* srcData, const int srcPitch);

	bool Preferred10BitOutput() const {
		return m_DisplayBitsPerChannel >= 10 && (m_InternalTexFmt == DXGI_FORMAT_R10G10B10A2_UNORM || m_InternalTexFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);
	}

	bool HandleHDRToggle();
	bool ToggleHDR(const DisplayConfig_t& displayConfig, const bool bEnableAdvancedColor);

public:
	HRESULT SetDevice(ID3D11Device *pDevice, ID3D11DeviceContext *pContext);
	HRESULT InitSwapChain(bool bWindowChanged);

	BOOL VerifyMediaType(const CMediaType* pmt) override;
	BOOL InitMediaType(const CMediaType* pmt) override;

	HRESULT InitializeD3D11VP(const FmtConvParams_t& params, const UINT width, const UINT height, const CMediaType* pmt);
	HRESULT InitializeTexVP(const FmtConvParams_t& params, const UINT width, const UINT height);
	void UpdatFrameProperties(); // use this after receiving modified frame from hardware decoder

	BOOL GetAlignmentSize(const CMediaType& mt, SIZE& Size) override;

	HRESULT ProcessSample(IMediaSample* pSample) override;
	REFERENCE_TIME m_sampleDuration = 0;
	HRESULT CopySample(IMediaSample* pSample);
	// Render: 1 - render first fied or progressive frame, 2 - render second fied, 0 or other - forced repeat of render.
	HRESULT Render(int field, const REFERENCE_TIME frameStartTime) override;
	HRESULT FillBlack() override;

	void SetVideoRect(const CRect& videoRect)      override;
	HRESULT SetWindowRect(const CRect& windowRect) override;
	HRESULT Reset(bool bDisplayModeChange) override;
	bool IsInit() const override { return m_bHdrDisplaySwitching; }

	HRESULT GetCurentImage(long *pDIBImage) override;
	HRESULT GetDisplayedImage(BYTE **ppDib, unsigned* pSize) override;
	HRESULT GetVPInfo(std::wstring& str) override;

	// Settings
	void Configure(const Settings_t& config) override;

	void SetRotation(int value) override;
	void SetStereo3dTransform(int value) override;

	void Flush() override;

	void ClearPreScaleShaders() override;
	void ClearPostScaleShaders() override;

	HRESULT AddPreScaleShader(const std::wstring& name, const std::string& srcCode) override;
	HRESULT AddPostScaleShader(const std::wstring& name, const std::string& srcCode) override;

	ISubPicAllocator* GetSubPicAllocator() override;

	void SwitchFullScreen(bool set) override;

	void SetPGSSubtitles(bool bPGSSubtitles);

private:
	void UpdateTexures();
	void UpdatePostScaleTexures();
	void UpdateUpscalingShaders();
	void UpdateDownscalingShaders();
	HRESULT UpdateConvertColorShader();
	void UpdateBitmapShader();

	HRESULT D3D11VPPass(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second);
	HRESULT ConvertColorPass(ID3D11Texture2D* pRenderTarget);
	HRESULT ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect,
		const CRect& dstRect, const int rotation, bool applyFlip = true, Tex2D_t* intermediate = nullptr);
	HRESULT FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect);

	bool DepthOverlayEnabled() const;
	bool BlendSourceDepthOverlay(ID3D11Texture2D* image, ID3D11ShaderResourceView* source,
		const CRect& picture, REFERENCE_TIME timestamp);
	void DrawDepthOverlay(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp);
	void DrawDepthView(ID3D11Texture2D* target, const CRect& picture, ID3D11ShaderResourceView* depth,
		SIZE textureSize, const CRect& source, int rotation, bool inverted);
	void DrawSubtitles(ID3D11Texture2D* pRenderTarget, const CRect* picture = nullptr);
	HRESULT Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, bool second, bool allowDlssSR = true, REFERENCE_TIME timestamp = INVALID_TIME, CTex2DRing* processingPostScale = nullptr, bool* overlayInputHandled = nullptr, const Tex2D_t* preparedSource = nullptr);
	void ConfigureJvrProjection(const Settings_t& config);
	void SetSvdLutStrength(int strength);
	void ResetSvdLutHistory();
	void ReleaseSvdLut(bool fast = false);
	std::wstring SvdLutStatus() const;
	HRESULT PrepareSvdLutDraw();
	HRESULT EnsureSvdLutPicture(DXGI_FORMAT format, UINT width, UINT height);
	bool SvdLutWorkAllowed();
	void BeginSvdLutScene(REFERENCE_TIME timestamp);
	void TakeSvdLutResult();
	void PollSvdLut();
	void CaptureSvdLut(REFERENCE_TIME timestamp, bool projected, bool second);
	const SvdLutScene* SvdLutSceneForPts(REFERENCE_TIME timestamp);
	struct SvdLutGrade {
		ID3D11PixelShader* shader;
		ID3D11ShaderResourceView* view;
		float blend;
	};
	std::optional<SvdLutGrade> PrepareSvdLutGrade(REFERENCE_TIME timestamp);
	HRESULT ApplySvdLutGrade(const SvdLutGrade& grade, const Tex2D_t& source, ID3D11Texture2D* target,
		const CRect& area, const CRect& visible, CPoint sourceOffset);
	HRESULT DrawCompletedSvdLut(const CompletedFrame& frame, ID3D11Texture2D* target,
		const CRect& picture, bool geometryChanging);
	void DrawSvdLut(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp);
	bool CanProjectJvr() const;
	HRESULT PrepareJvrFrame(const CRect& picture, bool second);
	HRESULT ProjectJvr(const Tex2D_t& source, ID3D11Texture2D* target, const CRect& sourceRect, const CRect& picture);
    bool DlssSRSourceAllowed(const CRect* preparedRect = nullptr) const;
	bool CanDlssSRUpscale(const CRect& srcRect, const CRect& dstRect, bool preparedRgb = false);
	Tex2D_t* PrepareDlssSR(const CRect& srcRect, const CRect& dstRect, bool afterMfg);
	Tex2D_t* EvaluateDlssSR(const Tex2D_t& source, Tex2D_t& input, const CRect& sourceRect, REFERENCE_TIME timestamp, float frameTimeMs, bool* overlayInputHandled = nullptr, bool depthPrepared = false);
	Tex2D_t* TryDlssSR(const CRect& srcRect, const CRect& dstRect, bool second, REFERENCE_TIME timestamp, bool* overlayInputHandled, const Tex2D_t* preparedSource);

	HRESULT AlphaBlt(ID3D11ShaderResourceView* pShaderResource, ID3D11Texture2D* pRenderTarget,
					 ID3D11Buffer* pVertexBuffer, D3D11_VIEWPORT* pViewPort,
					 ID3D11SamplerState* pSampler);
	HRESULT TextureCopyRect(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
							const CRect& srcRect, const CRect& destRect,
							ID3D11PixelShader* pPixelShader, ID3D11Buffer* pConstantBuffer,
							const int iRotation, const bool bFlip, const bool bLinear = false);

	HRESULT TextureResizeShader(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
								const CRect& srcRect, const CRect& destRect,
								ID3D11PixelShader* pPixelShader,
								const int iRotation, const bool bFlip);

	void UpdateStatsPresent();
	void UpdateStatsStatic() override;
	//void UpdateStatsPostProc();
	HRESULT DrawStats(ID3D11Texture2D* pRenderTarget, LONG featureOffset = 0);

public:
	// IMFVideoProcessor
	STDMETHODIMP SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues) override;

	// IMFVideoMixerBitmap
	STDMETHODIMP SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms) override;
	STDMETHODIMP UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms) override;
};
