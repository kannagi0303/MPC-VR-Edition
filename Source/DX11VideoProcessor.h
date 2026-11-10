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
#include <map>
#include "IVideoRenderer.h"
#include "DX11Helper.h"
#include "D3D11VP.h"
#include "D3DUtil/D3D11Font.h"
#include "D3DUtil/D3D11Geometry.h"
#include "VideoProcessor.h"
#include "SubPic/DX11SubPic.h"

#include <atomic>
#include <mutex>
#include "Mfg/Session.h"
#include "Mfg/FrameQueue.h"
#include "Mfg/LiveFlowGroups.h"
#include "Mfg/LiveFlowGroupPacing.h"
#include "Mfg/AdaptiveLookahead.h"
#include "Mfg/DisplaySubmitPacing.h"
#include "Mfg/DisplayPreparation.h"
#include "Mfg/AutoCadence.h"
#include "Mfg/PresentationThread.h"


class CVideoRendererInputPin;

class CDX11VideoProcessor
	: public CVideoProcessor
{
private:
	friend class CVideoRendererInputPin;
	bool m_bInterpolateAt50pct=true;
	bool m_bPGSSubtitles=false;
	bool SourceIsHDR10() const {return m_decExFmt.VideoTransferFunction==MFVideoTransFunc_2084 && m_decExFmt.VideoTransferMatrix!=DXVA2_VideoTransferMatrix_Unknown;}
	std::atomic<unsigned> m_mfgMultiplier{0};
    // User settings; changing either drains the MFG presentation worker.
    unsigned m_mfgProcessingMode = 0;  // Quality or Performance
	// Requested UI multiplier is independent of the display-budgeted effective multiplier.
	std::atomic<unsigned> m_mfgEffectiveMultiplier{0};
	unsigned m_mfgBudgetCandidate = 0;
	unsigned m_mfgBudgetStreak = 0;
	// Auto needs the *actual* interval PTS, including x1 fallback intervals.
	// Kept independent of Session so reducing to x1 does not lose cadence phase.
	int64_t m_mfgAutoEpoch = -1;
	int64_t m_mfgAutoLastPts = -1;
	mfg::Session m_mfg;
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
	bool QueueMfg(ID3D11Texture2D* image,REFERENCE_TIME timestamp,bool nativeInput=false,bool postVsrInput=false);
	bool PresentMfg();
	bool NextMfg(int64_t& deadline);
	void StopMfg(bool release=true);
    // Keep the old back buffer while a windowed resize is in progress.
    // Apply the last requested size on the source-render thread once it settles.
    bool m_resizePending = false;
    CRect m_resizeTarget;
    ULONGLONG m_resizeLastChangeTick = 0;
    HRESULT ApplyPendingWindowResize();

	HRESULT CompositeMfg(const mfg::FrameSurface& surface,REFERENCE_TIME timestamp,bool nativeInput);
    HRESULT RenderMfgVideo(const mfg::FrameSurface& surface, bool nativeInput, ID3D11Texture2D* target);
    HRESULT FinishMfgPresent(ID3D11Texture2D* preparedVideo, REFERENCE_TIME timestamp);
    HRESULT EnsureMfgPreparedTexture(ID3D11Texture2D* referenceBackBuffer);
    bool MfgPostVsrRequested() const;
    bool EnsureMfgPostVsr();
    void ResetMfgPostVsr();

    // Three-second, icon-only status badges. This state is only for presentation;
    // it does not change NGX, VSR or the existing statistics counters.
    std::atomic<bool> m_showFeatureBadges{true};
    std::mutex m_featureBadgeMutex;
    uint64_t m_badgeLastGenerated = 0;
    ULONGLONG m_badgeLastGeneratedTick = 0;
    ULONGLONG m_badgeVsrUntil = 0, m_badgeMfgUntil = 0;
    bool m_badgeVsrWasActive = false, m_badgeMfgWasActive = false;
    // A transient MFG pause or auto-cadence dip must not re-arm the badges.
    bool m_badgeVsrAnnounced = false, m_badgeMfgAnnounced = false;
    void ResetFeatureBadgeAnnouncements();
    HRESULT DrawFeatureBadges(ID3D11Texture2D* target);


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
    CD3D11Rectangle m_FeatureBadgeRect;

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
	unsigned GetVideoProcessorUse() override { return m_D3D11VP.IsReady() ? 1u : 0u; }
	std::wstring GetMfgStatus() override {
		const unsigned multiplier = m_mfgMultiplier.load();
		if (!multiplier) return L"Processor MFG: Off (NGX is not called)";
		if (m_mfgPresentFailed.load()) return L"Processor MFG: " + (multiplier==mfg::AutoMultiplier ? L"Auto" : L"x"+std::to_wstring(multiplier)) +
			L"; presenter failed, normal playback retained";
		const std::wstring& ngx = m_mfg.Status();
		return L"Processor MFG: " + (multiplier==mfg::AutoMultiplier ? L"Auto" : L"x"+std::to_wstring(multiplier)) + L"; " +
			(ngx.empty() ? L"NGX not yet attempted (waiting for eligible running frame)" : ngx) +
			L"; presents generated=" + std::to_wstring(m_mfgGeneratedPresents.load()) +
			L" real=" + std::to_wstring(m_mfgRealPresents.load()) +
			L" skipped=" + std::to_wstring(m_mfgSkipped.load());
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
	HRESULT ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const int rotation);
	HRESULT FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect);

	void DrawSubtitles(ID3D11Texture2D* pRenderTarget);
	HRESULT Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second);

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
	HRESULT DrawStats(ID3D11Texture2D* pRenderTarget);

public:
	// IMFVideoProcessor
	STDMETHODIMP SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues) override;

	// IMFVideoMixerBitmap
	STDMETHODIMP SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms) override;
	STDMETHODIMP UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms) override;
};
