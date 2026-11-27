// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Times.h"
#include "Mfg/CadenceTrace.h"
#include "DlssSR.h"
#include "DlssOpticalFlow.h"
#include "Depth/Depth.h"
#include "NgxRuntime.h"
#include "Mfg/MotionShaders.h"
#include "Edition/FixedShaderBytecode.h"
#include "SrMotionRefine.h"
#include "Helper.h"
#include "DX11Helper.h"
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <atomic>
#include <thread>
#include <exception>
#include <mutex>
#include <new>
#include <stop_token>
#include <array>
#include <filesystem>
#include <cwchar>
#include <cstdlib>
#include <shlobj.h>
#include <nvsdk_ngx.h>

namespace {

// The source-frame DLSS SR pipeline follows the design in HumbleUser33/MPCVR-Plus.
// NGX and the optical-flow shaders must not change the renderer's pipeline state.
class ContextStateScope
{
	ID3D11DeviceContext1* m_context;
	CComPtr<ID3DDeviceContextState> m_previous;

public:
	ContextStateScope(ID3D11DeviceContext1* context, ID3DDeviceContextState* state)
		: m_context(context)
	{
		if (m_context) m_context->SwapDeviceContextState(state, &m_previous);
	}

	~ContextStateScope()
	{
		if (m_context) m_context->SwapDeviceContextState(m_previous, nullptr);
	}
};

// Serialize D3D11 NGX calls across SR instances. Play first evaluates only
// when the lock is available.
std::mutex NgxD3D11Lock;

constexpr auto kProjectId = "a34c75e7-d396-4b6e-b429-c424fd42e453";

// A tiny GPU-side luminance thumbnail for conservative video hard-cut detection.
// No pixel-buffer readback is allowed to block the video or MFG presenter.
constexpr UINT kSceneWidth = 16;
constexpr UINT kSceneHeight = 9;
constexpr char kSceneThumbnailShader[] = R"(
Texture2D<float4> Source : register(t0);
float4 Main(float4 pos : SV_POSITION) : SV_TARGET {
    uint sw, sh;
    Source.GetDimensions(sw, sh);
    float luminance = 0.0;
    uint2 tile = uint2(pos.xy);
    [unroll] for (uint y = 0; y < 4; ++y) {
        [unroll] for (uint x = 0; x < 4; ++x) {
            float2 uv = (float2(tile) + (float2(x, y) + 0.5) / 4.0)
                / float2(16, 9);
            uint2 src = min(uint2(uv * float2(sw, sh)), uint2(sw - 1, sh - 1));
            luminance += dot(saturate(Source.Load(int3(src, 0)).rgb),
                float3(0.2126, 0.7152, 0.0722));
        }
    }
    return float4(luminance / 16.0, 0.0, 0.0, 1.0);
}
)";


} // namespace

struct CDlssSR::Session
{
	// NGX uses a private immediate context in Play first mode. Video conversion,
	// optical flow and depth keep using the renderer's original context.
	CComPtr<ID3D11Device1> ngxDevice;
	CComPtr<ID3D11DeviceContext> ngxContext;
	CComPtr<ID3D11DeviceContext4> renderSync, ngxSync;
	CComPtr<ID3D11Fence> renderFence, ngxFence;
	CComPtr<ID3D11Texture2D> ngxInput, ngxOutput, ngxDepth, ngxMotion;
	UINT64 fenceValue = 0;
	bool background = false;
	bool flowBound = false;
	bool validationShadersPrepared = false;
	CComPtr<ID3D11Device1> device;
	CComPtr<ID3D11DeviceContext1> context;
	CComPtr<ID3DDeviceContextState> state;
	CComPtr<ID3D11VertexShader> vertex;
	CComPtr<ID3D11PixelShader> luma, vectors, validatedVectors, costValidatedVectors;
	CComPtr<ID3D11PixelShader> refinedVectors, refinedCostVectors;
    bool depthActive = false;
    bool depthIsFlat = true;
	CComPtr<ID3D11Buffer> constants;
	CComPtr<ID3D11RenderTargetView> depthView, motionView;
	Tex2D_t input, output, depth, motion;
	bool validateMotion = false;
	bool costValidatedMotion = false;
	bool refinedValidatedMotion = false;
	bool refinedCostMotion = false;
	bool autoExposure = false;
	CDlssOpticalFlow flow;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	REFERENCE_TIME lastPts = -1;
	ULONGLONG rateStartMs = 0;
	unsigned rateFrames = 0;
	double inputFps = 0.0;
	bool initialized = false;
	bool validOutput = false;
	std::wstring failure;
	std::wstring activeStatus;
	int requestedModel = 0;
	UINT requestedInputWidth = 0, requestedInputHeight = 0;
	UINT requestedWidth = 0, requestedHeight = 0;
	UINT workingWidth = 0, workingHeight = 0;
	UINT fallbackWidth = 0, fallbackHeight = 0;
	bool sizeFailed = false;

	// GPU timing is diagnostic only: read back completed timestamps without
	// flushing/waiting on the immediate context, so playback pacing is unaffected.
	struct GpuSample {
		CComPtr<ID3D11Query> disjoint, start, end;
		bool pending = false;
	};
	std::array<GpuSample, 3> gpuSamples;
	bool gpuTimingAvailable = false;
	bool ngxGpuTimeValid = false;
	double ngxGpuMs = 0.0;
	ULONGLONG lastGpuSampleMs = 0;


    // Scene detection is best-effort and entirely separate from NGX timing.
    // A result must correspond to the immediately previous SR frame to reset
    // history; late results are discarded rather than stalling playback.
    struct SceneSample {
        CComPtr<ID3D11Texture2D> staging;
        CComPtr<ID3D11Query> ready;
        REFERENCE_TIME pts = -1;
        bool pending = false;
    };
    CComPtr<ID3D11PixelShader> sceneShader;
    CComPtr<ID3D11Texture2D> sceneTexture;
    CComPtr<ID3D11RenderTargetView> sceneRTV;
    std::array<SceneSample, 4> sceneSamples;
    std::array<BYTE, kSceneWidth * kSceneHeight> previousScene{};
    REFERENCE_TIME previousScenePts = -1;
    bool sceneEnabled = false;
    unsigned sceneCutCount = 0;

    void ResetSceneDetection()
    {
        previousScenePts = -1;
        for (auto& sample : sceneSamples) sample.pending = false;
    }

    void SetupSceneDetection()
    {
        sceneEnabled = false;
        sceneShader.Release();
        sceneTexture.Release();
        sceneRTV.Release();
        for (auto& sample : sceneSamples) {
            sample.staging.Release();
            sample.ready.Release();
        }
        ResetSceneDetection();
        CComPtr<ID3DBlob> code;
        if (FAILED((edition::CompileFixedShader<kSceneThumbnailShader,"Main","ps_5_0">(&code))) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(),
                code->GetBufferSize(), nullptr, &sceneShader))) return;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kSceneWidth;
        desc.Height = kSceneHeight;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &sceneTexture)) ||
            FAILED(device->CreateRenderTargetView(sceneTexture, nullptr, &sceneRTV))) return;

        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        for (auto& sample : sceneSamples) {
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &sample.staging)) ||
                FAILED(device->CreateQuery(&query, &sample.ready))) return;
        }
        sceneEnabled = true;
    }

    bool PollSceneDetection(REFERENCE_TIME lastFramePts, REFERENCE_TIME maxGap, CDepth* depthSource)
    {
        if (!sceneEnabled) return false;
        bool resetHistory = false;
        // Query slots may be completed in a different loop order. Consume by
        // increasing PTS; ignore stale samples and skipped frame intervals.
        for (;;) {
            SceneSample* earliest = nullptr;
            for (auto& sample : sceneSamples) {
                if (sample.pending && (!earliest || sample.pts < earliest->pts))
                    earliest = &sample;
            }
            if (!earliest) break;
            const HRESULT done = context->GetData(earliest->ready, nullptr, 0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (done == S_FALSE) break;
            if (FAILED(done)) { earliest->pending = false; continue; }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mappedResult = context->Map(earliest->staging, 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (mappedResult == DXGI_ERROR_WAS_STILL_DRAWING) break;
            if (FAILED(mappedResult)) { earliest->pending = false; continue; }
            std::array<BYTE, kSceneWidth * kSceneHeight> current{};
            for (UINT y = 0; y < kSceneHeight; ++y) {
                const auto* row = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
                for (UINT x = 0; x < kSceneWidth; ++x) current[y * kSceneWidth + x] = row[4 * x];
            }
            context->Unmap(earliest->staging, 0);
            const REFERENCE_TIME samplePts = earliest->pts;
            earliest->pending = false;
            if (samplePts <= previousScenePts) continue;
            if (previousScenePts >= 0 && samplePts - previousScenePts <= maxGap) {
                unsigned totalDifference = 0;
                unsigned changed = 0;
                unsigned structureDifference = 0;
                unsigned edges = 0;
                for (UINT y = 0; y < kSceneHeight; ++y) {
                    for (UINT x = 0; x < kSceneWidth; ++x) {
                        const size_t i = y * kSceneWidth + x;
                        const int now = int(current[i]);
                        const int before = int(previousScene[i]);
                        const int delta = std::abs(now - before);
                        totalDifference += unsigned(delta);
                        changed += unsigned(delta >= 72);
                        // Adjacent luminance differences capture image structure.
                        // A full-frame brightness flash can change every tile
                        // without substantially changing the spatial layout.
                        if (x) {
                            const size_t neighbor = i - 1;
                            const int nowEdge = now - int(current[neighbor]);
                            const int previousEdge = before - int(previousScene[neighbor]);
                            structureDifference += unsigned(std::abs(nowEdge - previousEdge));
                            ++edges;
                        }
                        if (y) {
                            const size_t neighbor = i - kSceneWidth;
                            const int nowEdge = now - int(current[neighbor]);
                            const int previousEdge = before - int(previousScene[neighbor]);
                            structureDifference += unsigned(std::abs(nowEdge - previousEdge));
                            ++edges;
                        }
                    }
                }
                // High-confidence cuts require a change of image structure,
                // not just a bright flash or fade. A second, stricter spatial
                // test can catch hard cuts between similarly exposed scenes.
                // Keep both tests conservative to protect DLSS temporal history.
                const bool strongCut =
                    totalDifference >= current.size() * 84 &&
                    changed * 100 >= current.size() * 70 &&
                    structureDifference >= edges * 35;
                const bool structuralCut =
                    totalDifference >= current.size() * 50 &&
                    changed * 100 >= current.size() * 40 &&
                    structureDifference >= edges * 65;
                // A confirmed cut may arrive late under GPU load. Clear
                // contaminated history while the cut is still recent.
                if (samplePts <= lastFramePts && lastFramePts - samplePts <= maxGap &&
                    (strongCut || structuralCut)) {
                    resetHistory = true;
                    if (depthSource) depthSource->MarkSceneCut(samplePts);
                    ++sceneCutCount;
                }
            }
            previousScene = current;
            previousScenePts = samplePts;
        }
        return resetHistory;
    }

    void SubmitSceneThumbnail(REFERENCE_TIME pts, ID3D11ShaderResourceView* source)
    {
        if (!sceneEnabled || pts < 0) return;
        SceneSample* freeSample = nullptr;
        for (auto& sample : sceneSamples) {
            if (!sample.pending) { freeSample = &sample; break; }
        }
        if (!freeSample) return; // Full GPU queue: skip sampling, never wait.
        const D3D11_VIEWPORT viewport{0, 0, float(kSceneWidth), float(kSceneHeight), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex, nullptr, 0);
        context->PSSetShader(sceneShader, nullptr, 0);
        context->PSSetShaderResources(0, 1, &source);
        context->OMSetRenderTargets(1, &sceneRTV.p, nullptr);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
        context->CopyResource(freeSample->staging, sceneTexture);
        context->End(freeSample->ready);
        freeSample->pts = pts;
        freeSample->pending = true;
    }

	void SetupGpuTiming()
	{
		gpuTimingAvailable = true;
		ngxGpuTimeValid = false;
		ngxGpuMs = 0.0;
		lastGpuSampleMs = 0;
		for (auto& sample : gpuSamples) {
			sample.pending = false;
			sample.disjoint.Release();
			sample.start.Release();
			sample.end.Release();
			D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
			if (FAILED(ngxDevice->CreateQuery(&desc, &sample.disjoint))) {
				gpuTimingAvailable = false;
				break;
			}
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(ngxDevice->CreateQuery(&desc, &sample.start)) ||
				FAILED(ngxDevice->CreateQuery(&desc, &sample.end))) {
				gpuTimingAvailable = false;
				break;
			}
		}
		if (!gpuTimingAvailable) {
			for (auto& sample : gpuSamples) {
				sample.disjoint.Release();
				sample.start.Release();
				sample.end.Release();
			}
		}
	}

	void PollGpuTiming()
	{
		if (!gpuTimingAvailable) return;
		constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
		for (auto& sample : gpuSamples) {
			if (!sample.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			const HRESULT ready = ngxContext->GetData(sample.disjoint, &disjoint, sizeof(disjoint), flags);
			if (ready == S_FALSE) continue;
			if (FAILED(ready)) { sample.pending = false; continue; }
			UINT64 begin = 0, end = 0;
			const HRESULT first = ngxContext->GetData(sample.start, &begin, sizeof(begin), flags);
			const HRESULT last = ngxContext->GetData(sample.end, &end, sizeof(end), flags);
			if (first == S_FALSE || last == S_FALSE) continue;
			sample.pending = false;
			if (SUCCEEDED(first) && SUCCEEDED(last) && !disjoint.Disjoint &&
				disjoint.Frequency && end >= begin) {
				ngxGpuMs = 1000.0 * double(end - begin) / double(disjoint.Frequency);
				ngxGpuTimeValid = true;
			}
		}
	}

	GpuSample* BeginGpuTiming()
	{
		if (!gpuTimingAvailable) return nullptr;
		const ULONGLONG now = GetTickCount64();
		if (lastGpuSampleMs && now - lastGpuSampleMs < 250) return nullptr;
		for (auto& sample : gpuSamples) {
			if (sample.pending) continue;
			ngxContext->Begin(sample.disjoint);
			ngxContext->End(sample.start);
			lastGpuSampleMs = now;
			return &sample;
		}
		return nullptr;
	}

	void EndGpuTiming(GpuSample* sample)
	{
		if (!sample) return;
		ngxContext->End(sample->end);
		ngxContext->End(sample->disjoint);
		sample->pending = true;
	}

	~Session()
	{
		if (initialized) {
			if (background && ngxFence && (fenceValue || feature) && SignalNgx()) {
				ngxFence->SetEventOnCompletion(fenceValue, nullptr);
			}
			ContextStateScope scope(background ? nullptr : context.p, state);
			std::lock_guard lock(NgxD3D11Lock);
			if (feature) {
				NVSDK_NGX_D3D11_ReleaseFeature(feature);
			}
			if (parameters) {
				NVSDK_NGX_D3D11_DestroyParameters(parameters);
			}
			NVSDK_NGX_D3D11_Shutdown1(ngxDevice);
		}
	}


	bool OpenInterop()
	{
		CComQIPtr<IDXGIDevice> dxgi(device.p);
		CComPtr<IDXGIAdapter> adapter;
		CComPtr<ID3D11Device> privateDevice;
		if (!dxgi || FAILED(dxgi->GetAdapter(&adapter))) return false;
		const auto level = device->GetFeatureLevel();
		if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
			&level, 1, D3D11_SDK_VERSION, &privateDevice, nullptr, &ngxContext)) ||
			FAILED(privateDevice.QueryInterface(&ngxDevice))) return false;

		CComQIPtr<ID3D11Device5> device5(device.p), ngxDevice5(ngxDevice.p);
		if (!device5 || !ngxDevice5 ||
			FAILED(context.QueryInterface(&renderSync)) || FAILED(ngxContext.QueryInterface(&ngxSync)) ||
			FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&renderFence)))) return false;
		HANDLE handle = nullptr;
		if (FAILED(renderFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle))) return false;
		const auto opened = ngxDevice5->OpenSharedFence(handle, IID_PPV_ARGS(&ngxFence));
		CloseHandle(handle);
		return SUCCEEDED(opened);
	}

	bool OpenTexture(Tex2D_t& texture, DXGI_FORMAT format, UINT width, UINT height,
		Tex2DType type, CComPtr<ID3D11Texture2D>& alias)
	{
		alias.Release();
		if (!background) return SUCCEEDED(texture.CheckCreate(device, format, width, height, type));
		texture.Release();
		auto desc = CreateTex2DDesc(format, width, height, type);
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture.pTexture)) ||
			FAILED(device->CreateShaderResourceView(texture.pTexture, nullptr, &texture.pShaderResource))) return false;
		texture.desc = desc;
		texture.type = type;
		CComQIPtr<IDXGIResource1> resource(texture.pTexture.p);
		HANDLE handle = nullptr;
		if (!resource || FAILED(resource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) return false;
		const auto opened = ngxDevice->OpenSharedResource1(handle, IID_PPV_ARGS(&alias));
		CloseHandle(handle);
		return SUCCEEDED(opened);
	}

	bool Open(ID3D11Device1* renderDevice, bool async)
	{
		const auto directory = std::filesystem::path(NgxRuntime::EditionDir());
		std::error_code error;
		if (!std::filesystem::is_regular_file(directory / L"nvngx_dlss.dll", error)) {
			failure = L"Inactive (missing edition/nvngx_dlss.dll)";
			return false;
		}
		wchar_t localAppData[MAX_PATH]{};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
			failure = L"Inactive (local application data unavailable)";
			return false;
		}
		const auto cache = std::filesystem::path(localAppData) / L"MPC-VR-MFG" / L"dlss-sr";
		std::filesystem::create_directories(cache, error);
		if (error) {
			failure = L"Inactive (NGX cache unavailable)";
			return false;
		}

		device = renderDevice;
		device->GetImmediateContext1(&context);
		const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
		const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
			? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
		if (FAILED(device->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
		                                           __uuidof(ID3D11Device), nullptr, &state))) {
			failure = L"Inactive (D3D11 context state unavailable)";
			return false;
		}
		background = async;
		if (background) {
			if (!OpenInterop()) {
				failure = L"Inactive (DLSS SR private D3D11 context unavailable)";
				return false;
			}
		} else {
			ngxDevice = device;
			ngxContext = context.p;
		}
		ContextStateScope scope(background ? nullptr : context.p, state);
		std::lock_guard lock(NgxD3D11Lock);
		const wchar_t* paths[] = {directory.c_str()};
		NVSDK_NGX_FeatureCommonInfo info{};
		info.PathListInfo.Path = paths;
		info.PathListInfo.Length = 1;
		const auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(
			kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "MPC-VR-MFG", cache.c_str(), ngxDevice, &info);
		if (NVSDK_NGX_FAILED(result)) {
			failure = std::format(L"Inactive (NGX init {:#x})", unsigned(result));
			return false;
		}
		initialized = true;
		int available = 0;
		const auto caps = NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters);
		if (NVSDK_NGX_FAILED(caps) || !parameters ||
		    NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
			int featureResult = 0;
			if (parameters) {
				parameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &featureResult);
			}
			failure = std::format(L"Inactive (NGX capability {:#x}, feature {:#x})", unsigned(caps), unsigned(featureResult));
			return false;
		}
		return true;
	}

	bool CreateForOutput(UINT width, UINT height, UINT outputWidth, UINT outputHeight, bool smallerOnly = false)
	{
		requestedInputWidth = width;
		requestedInputHeight = height;
		requestedWidth = outputWidth;
		requestedHeight = outputHeight;
		validOutput = false;

		// Keep the intermediate texture within the 8K video envelope even if the
		// requested presentation rectangle is larger. The renderer finishes scaling.
		if (width >= MaxOutputWidth || height >= MaxOutputHeight) {
			failure = L"Inactive (source exceeds the SR working size)";
			return false;
		}
		const auto targetWidth = std::min(outputWidth, MaxOutputWidth);
		const auto targetHeight = std::min(outputHeight, MaxOutputHeight);
		// A 1:1 request is DLAA, not an upscale. There is no smaller
		// intermediate output to retry and width+1 would violate clamp().
		if (width == targetWidth && height == targetHeight) {
			fallbackWidth = targetWidth;
			fallbackHeight = targetHeight;
			return !smallerOnly && Create(width, height, targetWidth, targetHeight);
		}
		// Ultra Performance normally uses a 3x upscale. If the runtime rejects
		// the full target, retry with a smaller intermediate at that scale.
		const double scale = std::min({1.0, 3.0 * width / targetWidth, 3.0 * height / targetHeight});
		fallbackWidth = std::clamp(UINT(targetWidth * scale), width + 1, targetWidth);
		fallbackHeight = std::clamp(UINT(targetHeight * scale), height + 1, targetHeight);
		if (smallerOnly) {
			return (fallbackWidth != targetWidth || fallbackHeight != targetHeight) &&
				Create(width, height, fallbackWidth, fallbackHeight);
		}
		return Create(width, height, targetWidth, targetHeight) || TrySmallerOutput(width, height);
	}

	bool TrySmallerOutput(UINT width, UINT height)
	{
		if (workingWidth == fallbackWidth && workingHeight == fallbackHeight) {
			return false;
		}
		return Create(width, height, fallbackWidth, fallbackHeight);
	}

	bool Create(UINT width, UINT height, UINT outputWidth, UINT outputHeight)
	{
		workingWidth = outputWidth;
		workingHeight = outputHeight;
		failure = background ? L"Inactive (DLSS SR shared texture setup failed)" :
			L"Inactive (DLSS SR texture or shader setup failed)";
		flowBound = false;
		ContextStateScope scope(background ? nullptr : context.p, state);
		if (feature) {
			if (background && (!SignalNgx() ||
				FAILED(ngxFence->SetEventOnCompletion(fenceValue, nullptr)))) {
				failure = L"Inactive (DLSS SR initialization fence failed)";
				return false;
			}
			std::lock_guard lock(NgxD3D11Lock);
			NVSDK_NGX_D3D11_ReleaseFeature(feature);
			feature = nullptr;
		}
		depthView.Release();
		motionView.Release();
		validateMotion = false;
		costValidatedMotion = false;
		refinedValidatedMotion = false;
		refinedCostMotion = false;
		autoExposure = false;
		lastPts = -1;
		validOutput = false;
		// Recreated NGX features require a fresh GPU timing sample.
		SetupGpuTiming();
		SetupSceneDetection();
		if (!OpenTexture(input, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, Tex2D_DefaultShaderRTarget, ngxInput) ||
		    !OpenTexture(output, DXGI_FORMAT_R16G16B16A16_FLOAT, outputWidth, outputHeight, Tex2D_DefaultShaderRTargetUAV, ngxOutput) ||
		    !OpenTexture(depth, DXGI_FORMAT_R32_FLOAT, width, height, Tex2D_DefaultShaderRTarget, ngxDepth) ||
		    !OpenTexture(motion, DXGI_FORMAT_R16G16_FLOAT, width, height, Tex2D_DefaultShaderRTarget, ngxMotion) ||
		    FAILED(device->CreateRenderTargetView(depth.pTexture, nullptr, &depthView)) ||
		    FAILED(device->CreateRenderTargetView(motion.pTexture, nullptr, &motionView))) {
			return false;
		}
		depthIsFlat = false;
		depthActive = false;

		if (!vertex) {
			CComPtr<ID3DBlob> vs, lumaCode, vectorsCode;
			if (FAILED((edition::CompileFixedShader<mfg::MotionShaders,"VS","vs_5_0">(&vs))) ||
			    FAILED((edition::CompileFixedShader<mfg::MotionShaders,"Luma","ps_5_0">(&lumaCode))) ||
			    FAILED((edition::CompileFixedShader<mfg::MotionShaders,"Vectors","ps_5_0">(&vectorsCode))) ||
			    FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex)) ||
			    FAILED(device->CreatePixelShader(lumaCode->GetBufferPointer(), lumaCode->GetBufferSize(), nullptr, &luma)) ||
			    FAILED(device->CreatePixelShader(vectorsCode->GetBufferPointer(), vectorsCode->GetBufferSize(), nullptr, &vectors))) {
				return false;
			}
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = 32;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			if (FAILED(device->CreateBuffer(&desc, nullptr, &constants))) {
				return false;
			}
		}
		// Compile the alternatives here so binding flow cannot compile shaders.
		if (!validationShadersPrepared) {
			const auto createShader = [&]<auto& Source, edition::FixedShaderString Entry>(CComPtr<ID3D11PixelShader>& shader) {
				if (shader) return;
				CComPtr<ID3DBlob> code;
				if (SUCCEEDED((edition::CompileFixedShader<Source,Entry,"ps_5_0">(&code)))) {
					device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
				}
			};
			createShader.operator()<sr_motion::RefineShaders,"VectorsValidatedSR">(refinedVectors);
			createShader.operator()<sr_motion::RefineShaders,"VectorsValidatedCostSR">(refinedCostVectors);
			createShader.operator()<mfg::MotionShaders,"VectorsValidated">(validatedVectors);
			createShader.operator()<mfg::MotionShaders,"VectorsValidatedCost">(costValidatedVectors);
			validationShadersPrepared = true;
		}

		const double scale = std::max(double(outputWidth) / width, double(outputHeight) / height);
		const bool dlaa = width == outputWidth && height == outputHeight;
		const auto quality = dlaa ? NVSDK_NGX_PerfQuality_Value_DLAA
			: scale <= 1.55 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
			: scale <= 1.85 ? NVSDK_NGX_PerfQuality_Value_Balanced
			: scale <= 2.25 ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		std::unique_lock lock(NgxD3D11Lock);
		parameters->Set(NVSDK_NGX_Parameter_Width, width);
		parameters->Set(NVSDK_NGX_Parameter_Height, height);
		parameters->Set(NVSDK_NGX_Parameter_OutWidth, outputWidth);
		parameters->Set(NVSDK_NGX_Parameter_OutHeight, outputHeight);
		const char* presetKey = dlaa ? NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA
			: NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
		int autoPreset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
		switch (quality) {
		case NVSDK_NGX_PerfQuality_Value_Balanced:
			presetKey = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
			break;
		case NVSDK_NGX_PerfQuality_Value_MaxPerf:
			presetKey = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
			autoPreset = NVSDK_NGX_DLSS_Hint_Render_Preset_M;
			break;
		case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
			presetKey = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
			autoPreset = NVSDK_NGX_DLSS_Hint_Render_Preset_L;
			break;
		default:
			break;
		}
		const int preset = autoPreset;
		parameters->Set(presetKey, preset);
		parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, int(quality));
		// The video decoder supplies SDR color, but no camera exposure buffer.
		// Let NGX estimate exposure instead of assuming it is valid for every
		// scene; fall back to the existing path if this option is rejected.
		const int basicFlags = int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
			basicFlags | int(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
		auto result = NVSDK_NGX_D3D11_CreateFeature(ngxContext, NVSDK_NGX_Feature_SuperSampling, parameters, &feature);
		if (NVSDK_NGX_FAILED(result) || !feature) {
			if (feature) {
				NVSDK_NGX_D3D11_ReleaseFeature(feature);
				feature = nullptr;
			}
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, basicFlags);
			result = NVSDK_NGX_D3D11_CreateFeature(ngxContext, NVSDK_NGX_Feature_SuperSampling, parameters, &feature);
		} else {
			autoExposure = true;
		}
		if (NVSDK_NGX_FAILED(result) || !feature) {
			failure = std::format(L"Inactive (NGX create {:#x})", unsigned(result));
			return false;
		}
		requestedModel = preset;
		activeStatus = std::format(L"Active ({}Auto, requested model {})",
			dlaa ? L"DLAA, " : L"",
			wchar_t(L'J' + preset - NVSDK_NGX_DLSS_Hint_Render_Preset_J));
		if (autoExposure) {
			activeStatus += L", auto exposure";
		}
		if (sceneEnabled) {
			activeStatus += L", scene-cut guard";
		}
		if (outputWidth != requestedWidth || outputHeight != requestedHeight) {
			activeStatus += std::format(L", intermediate {}x{}", outputWidth, outputHeight);
		}
		lock.unlock();
		return !background || PrimeFeature();
	}

	void SetEvaluationParameters(bool reset, float frameTimeMs)
	{
		parameters->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(background ? ngxInput.p : input.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(background ? ngxOutput.p : output.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(background ? ngxDepth.p : depth.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(background ? ngxMotion.p : motion.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_Reset, int(reset));
		parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
		parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, input.desc.Width);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, input.desc.Height);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, frameTimeMs);
	}

	bool PrimeFeature()
	{
		// Warm up lazy NGX work on a synthetic input. No playback frame is consumed.
		const FLOAT black[4] = {};
		const FLOAT flat[4] = {0.5f, 0, 0, 0};
		CComPtr<ID3D11RenderTargetView> color, motionView, depthView;
		if (FAILED(ngxDevice->CreateRenderTargetView(ngxInput, nullptr, &color)) ||
			FAILED(ngxDevice->CreateRenderTargetView(ngxMotion, nullptr, &motionView)) ||
			FAILED(ngxDevice->CreateRenderTargetView(ngxDepth, nullptr, &depthView))) {
			failure = L"Inactive (DLSS SR warmup texture setup failed)";
			return false;
		}
		ngxContext->ClearRenderTargetView(color, black);
		ngxContext->ClearRenderTargetView(motionView, black);
		ngxContext->ClearRenderTargetView(depthView, flat);
		std::lock_guard lock(NgxD3D11Lock);
		SetEvaluationParameters(true, 33.3f);
		const auto result = NVSDK_NGX_D3D11_EvaluateFeature(ngxContext, feature, parameters, nullptr);
		if (NVSDK_NGX_FAILED(result)) {
			failure = std::format(L"Inactive (NGX prime {:#x})", unsigned(result));
			return false;
		}
		// The first real frame resets NGX history again.
		return true;
	}

	bool SignalNgx()
	{
		const auto value = fenceValue + 1;
		if (FAILED(ngxSync->Signal(ngxFence, value))) return false;
		fenceValue = value;
		ngxContext->Flush();
		return true;
	}

	bool BindFlow()
	{
		if (flowBound) return true;
		ContextStateScope scope(context, state);
		CDlssOpticalFlow::Options options;
		if (!flow.IsReady() || flow.Width() != requestedInputWidth || flow.Height() != requestedInputHeight) {
			if (!flow.Init(device, context, requestedInputWidth, requestedInputHeight, options)) {
				options.cost = false;
				if (!flow.Init(device, context, requestedInputWidth, requestedInputHeight, options)) {
					failure = L"Inactive (NVIDIA Optical Flow unavailable)";
					return false;
				}
			}
		}
		flow.Reset();
		refinedValidatedMotion = flow.Bidirectional() && flow.GridSize() == 4 && refinedVectors;
		validateMotion = flow.Bidirectional() && (refinedValidatedMotion || validatedVectors);
		refinedCostMotion = refinedValidatedMotion && refinedCostVectors;
		costValidatedMotion = validateMotion && flow.HasCost() && (refinedCostMotion || costValidatedVectors);
		if (flow.HasCost() && !costValidatedMotion) {
			options.cost = false;
			if (!flow.Init(device, context, requestedInputWidth, requestedInputHeight, options)) {
				failure = L"Inactive (NVIDIA Optical Flow fallback unavailable)";
				return false;
			}
		}
		activeStatus += std::format(L", optical flow {}x{}", flow.GridSize(), flow.GridSize());
		if (validateMotion) {
			activeStatus += costValidatedMotion ? L", cost-aware motion" : L", validated motion";
			if (costValidatedMotion ? refinedCostMotion : refinedValidatedMotion) activeStatus += L", edge-aware grid";
		}
		flowBound = true;
		return true;
	}

	bool Matches(ID3D11Device1* renderDevice, UINT width, UINT height,
		UINT outWidth, UINT outHeight, bool async) const
	{
		return device.p == renderDevice && background == async &&
			requestedInputWidth == width && requestedInputHeight == height &&
			requestedWidth == outWidth && requestedHeight == outHeight;
	}

	bool Evaluate(REFERENCE_TIME pts, float frameTimeMs, CDepth* depthSource, bool sourceFrame,
		ID3D11ShaderResourceView* cleanSource, std::unique_lock<std::mutex>& ngxLock)
	{
		if (validOutput && pts >= 0 && pts == lastPts) {
			return true;
		}
		// Drop NGX/NVOFA history after a substantial timestamp gap. A fixed
		// 250 ms threshold can reuse stale motion after several missing 60 fps
		// frames; use the source cadence with a conservative 100-250 ms window.
		// This is a discontinuity guard, not a content-based scene-cut detector.
		const REFERENCE_TIME expectedInterval =
			(frameTimeMs > 0.f && frameTimeMs < 250.f)
			? REFERENCE_TIME(frameTimeMs * 10000.f) : REFERENCE_TIME(0);
		const REFERENCE_TIME maxContinuousGap = std::clamp<REFERENCE_TIME>(
			expectedInterval * 3, 1000000LL, 2500000LL);
		const bool discontinuity = !validOutput || pts < 0 || lastPts < 0 || pts < lastPts ||
			pts - lastPts > maxContinuousGap;
		// Scene results arrive asynchronously, generally one frame after the
		// cut. Never wait for a result. A recent late cut can still clear history.
		const bool sceneCut = !discontinuity && PollSceneDetection(lastPts, maxContinuousGap,
            sourceFrame ? depthSource : nullptr);
		const bool reset = discontinuity || sceneCut;
		if (reset) {
			flow.Reset();
			if (discontinuity) ResetSceneDetection();
		} else {
			frameTimeMs = float((pts - lastPts) / 10000.0);
		}
		ContextStateScope scope(context, state);
		context->ClearState();
		const UINT w = input.desc.Width, h = input.desc.Height;
		const D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertex, nullptr, 0);
		bool hasMotion = false;
		ID3D11ShaderResourceView* empty[4] = {};
		context->PSSetShader(luma, nullptr, 0);
		context->PSSetShaderResources(0, 1, &input.pShaderResource.p);
		auto* target = flow.FrameTarget();
		context->OMSetRenderTargets(1, &target, nullptr);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->PSSetShaderResources(0, 4, empty);
		hasMotion = flow.Execute();
		if (flow.LastExecuteFailed()) {
			failure = L"Inactive (NVIDIA Optical Flow execution failed)";
			return false;
		}
		if (hasMotion) {
			const UINT values[8] = {w, h, w, h, flow.GridSize(), 0, 0, 0};
			context->UpdateSubresource(constants, 0, nullptr, values, 0, 0);
			context->PSSetConstantBuffers(0, 1, &constants.p);
			auto* flowView = flow.ForwardFlow();
			context->PSSetShaderResources(1, 1, &flowView);
			if (validateMotion) {
				auto* reverseView = flow.BackwardFlow();
				context->PSSetShaderResources(2, 1, &reverseView);
				if (costValidatedMotion) {
					auto* costView = flow.ForwardCost();
					context->PSSetShaderResources(3, 1, &costView);
					context->PSSetShader(refinedCostMotion ? refinedCostVectors.p : costValidatedVectors.p, nullptr, 0);
				} else {
					context->PSSetShader(refinedValidatedMotion ? refinedVectors.p : validatedVectors.p, nullptr, 0);
				}
			} else {
				context->PSSetShader(vectors, nullptr, 0);
			}
			context->OMSetRenderTargets(1, &motionView.p, nullptr);
			context->Draw(3, 0);
		} else {
			const FLOAT zero[4] = {};
			context->ClearRenderTargetView(motionView, zero);
		}
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->PSSetShaderResources(0, 4, empty);
		depthActive = false;
		if (depthSource) {
			const RECT picture{0, 0, LONG(w), LONG(h)};
			const SIZE modelSize{LONG(w), LONG(h)};
			if (sourceFrame && !cleanSource) {
				depthSource->Update(device, context, input.pShaderResource, picture, modelSize, pts);
			} else {
				depthSource->PollCompleted();
			}
			if (!reset && hasMotion) {
				depthActive = depthSource->DrawEstimate(context, depth.pTexture, depthView, picture, modelSize, pts, frameTimeMs, false);
			}
		}
		if (depthActive) {
			depthIsFlat = false;
		} else if (!depthIsFlat) {
			const FLOAT flatDepth[4] = {0.5f, 0, 0, 0};
			context->ClearRenderTargetView(depthView, flatDepth);
			depthIsFlat = true;
		}
		SubmitSceneThumbnail(pts, cleanSource ? cleanSource : input.pShaderResource.p);
		SetEvaluationParameters(reset || !hasMotion, frameTimeMs);
		if (background) {
			// The renderer writes color/flow/depth, then the SR device reads them.
			const auto value = fenceValue + 1;
			if (FAILED(renderSync->Signal(renderFence, value))) {
				failure = L"Inactive (DLSS SR input signal failed)";
				return false;
			}
			context->Flush();
			fenceValue = value;
			if (FAILED(ngxSync->Wait(ngxFence, value))) {
				failure = L"Inactive (DLSS SR input fence failed)";
				return false;
			}
		}
		PollGpuTiming();
		GpuSample* gpuSample = BeginGpuTiming();
		const auto result = NVSDK_NGX_D3D11_EvaluateFeature(ngxContext, feature, parameters, nullptr);
		ngxLock.unlock();
		EndGpuTiming(gpuSample);
		if (background) {
			// Queue the renderer's output read behind NGX completion on the GPU.
			const auto value = fenceValue + 1;
			if (FAILED(ngxSync->Signal(ngxFence, value))) {
				failure = L"Inactive (DLSS SR output signal failed)";
				return false;
			}
			fenceValue = value;
			ngxContext->Flush();
			if (FAILED(renderSync->Wait(renderFence, value))) {
				failure = L"Inactive (DLSS SR output fence failed)";
				return false;
			}
		}
		if (NVSDK_NGX_FAILED(result)) {
			failure = std::format(L"Inactive (NGX evaluate {:#x})", unsigned(result));
			return false;
		}
		if (depthSource) depthSource->RecordSrEvaluation(depthActive);
		const auto now = GetTickCount64();
		if (!validOutput) {
			rateStartMs = now;
			rateFrames = 0;
			inputFps = 0.0;
		} else {
			++rateFrames;
			const auto elapsedMs = now - rateStartMs;
			if (elapsedMs >= 1000) {
				inputFps = rateFrames * 1000.0 / elapsedMs;
				rateStartMs = now;
				rateFrames = 0;
			}
		}
		lastPts = pts;
		validOutput = true;
		return true;
	}
};

struct CDlssSR::Preparation {
	CComPtr<ID3D11Device1> device;
	UINT width = 0, height = 0, outputWidth = 0, outputHeight = 0;
	bool discard = false; // Accessed only under RendererLock.
	std::unique_ptr<Session> session;
	std::atomic<bool> finished{false};
	std::jthread worker;
};

CDlssSR::CDlssSR() = default;
CDlssSR::~CDlssSR()
{
    // UFE-5: a background NGX preparation can be inside a multi-second,
    // non-interruptible driver call. Joining that jthread on the DirectShow
    // graph thread would stall MPC-BE when switching videos or quitting.
    // Request cooperative cancellation first. If the worker is still active,
    // let a self-contained cleanup thread join it and release its COM/NGX
    // resources. The cleanup thread pins this AX module until all code and
    // destructors have completed, avoiding unload/use-after-free races.
    if (m_preparation && !m_preparation->finished.load(std::memory_order_acquire)) {
        m_preparation->worker.request_stop();
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&NgxD3D11Lock), &module)) {
            struct DeferredPreparation {
                std::unique_ptr<Preparation> preparation;
                HMODULE module = nullptr;

                static DWORD WINAPI Run(LPVOID context) noexcept
                {
                    auto* cleanup = static_cast<DeferredPreparation*>(context);
                    const HMODULE pinnedModule = cleanup->module;
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    // Preparation's jthread joins BEFORE its Session is
                    // destroyed. This is the original safe ownership order.
                    delete cleanup;
                    if (SUCCEEDED(com)) CoUninitialize();
                    // FreeLibrary() followed by returning would execute code
                    // in an unloaded DLL. This API performs both atomically.
                    FreeLibraryAndExitThread(pinnedModule, 0);
                    return 0;
                }
            };
            std::unique_ptr<DeferredPreparation> deferred(new (std::nothrow) DeferredPreparation);
            if (deferred) {
                deferred->module = module;
                deferred->preparation = std::move(m_preparation);
                HANDLE thread = CreateThread(nullptr, 0, &DeferredPreparation::Run,
                    deferred.get(), 0, nullptr);
                if (thread) {
                    deferred.release(); // Ownership transferred to Run().
                    CloseHandle(thread);
                } else {
                    m_preparation = std::move(deferred->preparation);
                    FreeLibrary(module);
                }
            } else {
                FreeLibrary(module);
            }
        }
    }
    m_preparation.reset();
    m_session.reset();
}

Tex2D_t* CDlssSR::Prepare(ID3D11Device1* device, UINT width, UINT height,
	UINT outputWidth, UINT outputHeight, bool background)
{
	if (m_preparation) {
		if (!m_preparation->finished.load(std::memory_order_acquire)) return nullptr;
		const auto& prepared = m_preparation->session;
		const bool matches = !m_preparation->discard && background &&
			m_preparation->device.p == device && m_preparation->width == width &&
			m_preparation->height == height && m_preparation->outputWidth == outputWidth &&
			m_preparation->outputHeight == outputHeight;
		if (matches && prepared && !prepared->sizeFailed) {
			const auto completed = prepared->ngxFence->GetCompletedValue();
			if (completed == UINT64_MAX) {
				prepared->sizeFailed = true;
				prepared->failure = L"Inactive (DLSS SR device removed during preparation)";
			} else if (completed < prepared->fenceValue) {
				return nullptr;
			}
		}
		if (!matches && prepared) {
			auto retired = std::move(m_preparation->session);
			m_preparation.reset();
			StartPreparation(std::move(retired), background ? device : nullptr,
				width, height, outputWidth, outputHeight, false);
			return nullptr;
		}
		m_session = std::move(m_preparation->session);
		m_preparation.reset();
		if (matches && !m_session) {
			m_unavailable = true;
			m_failure = L"Inactive (DLSS SR background preparation failed)";
		}
	}
	if (m_unavailable) return nullptr;
	if (background && (!m_session || !m_session->Matches(device, width, height, outputWidth, outputHeight, true)
		|| m_retrySmaller)) {
		const bool smallerOnly = m_retrySmaller && m_session &&
			m_session->Matches(device, width, height, outputWidth, outputHeight, true);
		m_retrySmaller = false;
		StartPreparation(std::move(m_session), device, width, height, outputWidth, outputHeight, smallerOnly);
		return nullptr;
	}
	if (!background && m_session && (m_session->background || m_session->device.p != device)) {
		Reset();
		if (m_preparation) return nullptr;
	}
	if (!m_session) {
		m_session = std::make_unique<Session>();
		if (!OpenSession(*m_session, device, false)) {
			m_failure = m_session->failure;
			m_session.reset();
			m_unavailable = true;
			return nullptr;
		}
	}
	auto& session = *m_session;
	if (!background && !session.Matches(device, width, height, outputWidth, outputHeight, false)) {
		CreateSession(session, width, height, outputWidth, outputHeight, false);
	}
	if (!session.sizeFailed && !session.flowBound) {
		const auto start = GetPreciseTick();
		session.sizeFailed = !session.BindFlow();
		const auto now = GetPreciseTick();
		mfg::SourceCadenceTrace().Record("sr_bind_flow", now, -1, -1, 0, 0, 0, 0, 0, false, 0,
			session.sizeFailed ? E_FAIL : S_OK, now - start);
	}
	if (session.sizeFailed) {
		m_failure = session.failure.empty() ? L"Inactive (DLSS SR preparation failed)" : session.failure;
		return nullptr;
	}
	m_failure.clear();
	return &session.input;
}

bool CDlssSR::OpenSession(Session& session, ID3D11Device1* device, bool background)
{
	const auto start = GetPreciseTick();
	const bool opened = session.Open(device, background);
	const auto now = GetPreciseTick();
	mfg::SourceCadenceTrace().Record("sr_open", now, -1, -1, 0, 0, 0, 0, 0, false, 0,
		opened ? S_OK : E_FAIL, now - start);
	return opened;
}

void CDlssSR::CreateSession(Session& session, UINT width, UINT height, UINT outputWidth,
	UINT outputHeight, bool smallerOnly)
{
	const auto start = GetPreciseTick();
	session.sizeFailed = !session.CreateForOutput(width, height, outputWidth, outputHeight, smallerOnly);
	const auto now = GetPreciseTick();
	mfg::SourceCadenceTrace().Record("sr_create", now, -1, -1, 0, 0, 0, 0, 0, false, 0,
		session.sizeFailed ? E_FAIL : S_OK, now - start);
}

void CDlssSR::StartPreparation(std::unique_ptr<Session> retired, ID3D11Device1* device,
	UINT width, UINT height, UINT outputWidth, UINT outputHeight, bool smallerOnly)
{
	// A session from Wait for Extended owns the playback immediate context.
	// Retire that session on the serialized renderer path.
	if (retired && !retired->background) retired.reset();
	if (retired) retired->flow.Release();
	try {
		// Any previous job was already finished before its result was taken.
		auto job = std::make_unique<Preparation>();
		job->device = device;
		job->width = width;
		job->height = height;
		job->outputWidth = outputWidth;
		job->outputHeight = outputHeight;
		job->session = std::move(retired);
		job->worker = std::jthread([pending = job.get(), smallerOnly](std::stop_token stop) {
			const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			pending->session.reset();
			if (pending->device && SUCCEEDED(com) && !stop.stop_requested()) {
				try {
					pending->session = std::make_unique<Session>();
					auto& session = *pending->session;
					if (OpenSession(session, pending->device, true)) {
                    // An in-flight NGX call cannot be interrupted. However,
                    // do not start another expensive call once cancellation
                    // has been observed between initialization stages.
                    if (!stop.stop_requested()) {
                        CreateSession(session, pending->width, pending->height, pending->outputWidth,
                            pending->outputHeight, smallerOnly);
                    } else {
                        session.sizeFailed = true;
                    }
                    if (!stop.stop_requested() && !session.sizeFailed && !session.SignalNgx()) {
                        session.failure = L"Inactive (DLSS SR initialization fence failed)";
                        session.sizeFailed = true;
                    }
					} else {
						session.sizeFailed = true;
					}
					// Retain the request key even when init failed so the same
					// input cannot repeatedly launch preparation.
					session.device = pending->device;
					session.background = true;
					session.requestedInputWidth = pending->width;
					session.requestedInputHeight = pending->height;
					session.requestedWidth = pending->outputWidth;
					session.requestedHeight = pending->outputHeight;
				} catch (...) {
					pending->session.reset();
				}
			}
			if (SUCCEEDED(com)) CoUninitialize();
			pending->finished.store(true, std::memory_order_release);
		});
		m_preparation = std::move(job);
		m_failure.clear();
	} catch (const std::exception&) {
		m_unavailable = true;
		m_failure = L"Inactive (DLSS SR background preparation unavailable)";
	}
}

Tex2D_t* CDlssSR::Evaluate(int64_t pts, float frameTimeMs, CDepth* depthSource, bool sourceFrame, ID3D11ShaderResourceView* cleanSource)
{
	std::unique_lock lock(NgxD3D11Lock, std::defer_lock);
	if (m_session->background) {
		if (!lock.try_lock()) {
			ResetHistory();
			return nullptr;
		}
	} else {
		lock.lock();
	}
	const bool trace = !m_session->validOutput && mfg::SourceCadenceTrace().Enabled();
	const auto start = trace ? GetPreciseTick() : 0;
	const bool evaluated = m_session->Evaluate(pts, frameTimeMs, depthSource, sourceFrame, cleanSource, lock);
	if (trace) {
		const auto now = GetPreciseTick();
		mfg::SourceCadenceTrace().Record("sr_first_output", now, pts, -1, 0, 0, 0, 0, 0, false, 0,
			evaluated ? S_OK : E_FAIL, now - start);
	}
	if (!evaluated) {
		if (lock.owns_lock()) lock.unlock();
		auto& session = *m_session;
		if (session.workingWidth != session.fallbackWidth || session.workingHeight != session.fallbackHeight) {
			if (session.background) {
				m_retrySmaller = true;
			} else if (session.TrySmallerOutput(session.requestedInputWidth, session.requestedInputHeight) &&
				session.BindFlow()) {
				m_failure.clear();
				return nullptr;
			}
		}
		m_failure = session.failure;
		session.validOutput = false;
		session.sizeFailed = true;
		return nullptr;
	}
	return &m_session->output;
}

void CDlssSR::ResetHistory()
{
	if (m_session) {
		m_session->validOutput = false;
		m_session->flow.Reset();
		m_session->ResetSceneDetection();
        m_session->depthActive = false;
	}
}

void CDlssSR::Reset()
{
	m_unavailable = false;
	m_failure.clear();
	m_retrySmaller = false;
	if (m_preparation) m_preparation->discard = true;
	if (m_session && m_session->background) {
		StartPreparation(std::move(m_session), nullptr, 0, 0, 0, 0, false);
	} else {
		m_session.reset();
	}
}

bool CDlssSR::Preparing() const
{
	return m_preparation && m_preparation->device && !m_preparation->discard;
}

bool CDlssSR::Active() const
{
	return m_session && m_session->validOutput;
}

CDlssSR::Stats CDlssSR::GetStats() const
{
	if (!Active()) {
		return {};
	}
	return {m_session->input.desc.Width, m_session->input.desc.Height,
		m_session->output.desc.Width, m_session->output.desc.Height, m_session->inputFps,
		m_session->ngxGpuMs, m_session->ngxGpuTimeValid};
}

int CDlssSR::RequestedModel() const
{
	return Active() ? m_session->requestedModel : 0;
}

const wchar_t* CDlssSR::Status(const CDepth* depthSource) const
{
	if (Active()) {
        if (depthSource) {
            m_statusDetails = m_session->activeStatus + L", depth DirectML";
            if (m_session->depthActive) {
                m_statusDetails += depthSource->ForFrame(m_session->lastPts).view
                    ? L", estimated depth active" : L", reprojecting depth";
            } else {
                m_statusDetails += std::format(L", flat ({})", depthSource->Status());
            }
            return m_statusDetails.c_str();
        }
		return m_session->activeStatus.c_str();
	}
	if (Preparing()) return L"Preparing DLSS SR";
	return m_failure.empty() ? L"Inactive (waiting for an eligible SDR upscale)" : m_failure.c_str();
}
