// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "DlssSR.h"
#include "DlssOpticalFlow.h"
#include "ZipDepth.h"
#include "NgxRuntime.h"
#include "Mfg/MotionShaders.h"
#include "SrMotionRefine.h"
#include "Helper.h"
#include "DX11Helper.h"
#include <d3dcompiler.h>
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
		m_context->SwapDeviceContextState(state, &m_previous);
	}

	~ContextStateScope()
	{
		m_context->SwapDeviceContextState(m_previous, nullptr);
	}
};

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
	CComPtr<ID3D11Device1> device;
	CComPtr<ID3D11DeviceContext1> context;
	CComPtr<ID3DDeviceContextState> state;
	CComPtr<ID3D11VertexShader> vertex;
	CComPtr<ID3D11PixelShader> luma, vectors, validatedVectors, costValidatedVectors;
    CZipDepthHistory zipHistory;
    bool zipDepthActive = false;
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
        if (FAILED(D3DCompile(kSceneThumbnailShader, sizeof(kSceneThumbnailShader) - 1,
            nullptr, nullptr, nullptr, "Main", "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) ||
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

    bool PollSceneDetection(REFERENCE_TIME lastFramePts, REFERENCE_TIME maxGap, CZipDepth* zipDepth)
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
                    if (zipDepth) zipDepth->MarkSceneCut(samplePts);
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
			if (FAILED(device->CreateQuery(&desc, &sample.disjoint))) {
				gpuTimingAvailable = false;
				break;
			}
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, &sample.start)) ||
				FAILED(device->CreateQuery(&desc, &sample.end))) {
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
			const HRESULT ready = context->GetData(sample.disjoint, &disjoint, sizeof(disjoint), flags);
			if (ready == S_FALSE) continue;
			if (FAILED(ready)) { sample.pending = false; continue; }
			UINT64 begin = 0, end = 0;
			const HRESULT first = context->GetData(sample.start, &begin, sizeof(begin), flags);
			const HRESULT last = context->GetData(sample.end, &end, sizeof(end), flags);
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
			context->Begin(sample.disjoint);
			context->End(sample.start);
			lastGpuSampleMs = now;
			return &sample;
		}
		return nullptr;
	}

	void EndGpuTiming(GpuSample* sample)
	{
		if (!sample) return;
		context->End(sample->end);
		context->End(sample->disjoint);
		sample->pending = true;
	}

	~Session()
	{
		if (initialized) {
			ContextStateScope scope(context, state);
			if (feature) {
				NVSDK_NGX_D3D11_ReleaseFeature(feature);
			}
			if (parameters) {
				NVSDK_NGX_D3D11_DestroyParameters(parameters);
			}
			NVSDK_NGX_D3D11_Shutdown1(device);
		}
	}


	bool Open(ID3D11Device1* renderDevice)
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
		ContextStateScope scope(context, state);
		const wchar_t* paths[] = {directory.c_str()};
		NVSDK_NGX_FeatureCommonInfo info{};
		info.PathListInfo.Path = paths;
		info.PathListInfo.Length = 1;
		const auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(
			kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "MPC-VR-MFG", cache.c_str(), device, &info);
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

	bool CreateForOutput(UINT width, UINT height, UINT outputWidth, UINT outputHeight)
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
			return Create(width, height, targetWidth, targetHeight);
		}
		// Ultra Performance normally uses a 3x upscale. If the runtime rejects
		// the full target, retry with a smaller intermediate at that scale.
		const double scale = std::min({1.0, 3.0 * width / targetWidth, 3.0 * height / targetHeight});
		fallbackWidth = std::clamp(UINT(targetWidth * scale), width + 1, targetWidth);
		fallbackHeight = std::clamp(UINT(targetHeight * scale), height + 1, targetHeight);
		return Create(width, height, targetWidth, targetHeight) ||
			TrySmallerOutput(width, height);
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
		failure = L"Inactive (DLSS SR texture or shader setup failed)";
		ContextStateScope scope(context, state);
		if (feature) {
			NVSDK_NGX_D3D11_ReleaseFeature(feature);
			feature = nullptr;
		}
		depthView.Release();
		motionView.Release();
		validateMotion = false;
		costValidatedMotion = false;
		refinedValidatedMotion = false;
		refinedCostMotion = false;
		validatedVectors.Release();
		costValidatedVectors.Release();
		autoExposure = false;
		lastPts = -1;
		validOutput = false;
		// Recreated NGX features require a fresh GPU timing sample.
		SetupGpuTiming();
		SetupSceneDetection();
		if (FAILED(input.CheckCreate(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, Tex2D_DefaultShaderRTarget)) ||
		    FAILED(output.CheckCreate(device, DXGI_FORMAT_R16G16B16A16_FLOAT, outputWidth, outputHeight, Tex2D_DefaultShaderRTargetUAV)) ||
		    FAILED(depth.CheckCreate(device, DXGI_FORMAT_R32_FLOAT, width, height, Tex2D_DefaultShaderRTarget)) ||
		    FAILED(motion.CheckCreate(device, DXGI_FORMAT_R16G16_FLOAT, width, height, Tex2D_DefaultShaderRTarget)) ||
		    FAILED(device->CreateRenderTargetView(depth.pTexture, nullptr, &depthView)) ||
		    FAILED(device->CreateRenderTargetView(motion.pTexture, nullptr, &motionView))) {
			return false;
		}
		const FLOAT flatDepth[4] = {0.5f, 0, 0, 0};
		context->ClearRenderTargetView(depthView, flatDepth);
		depthIsFlat = true;
		zipHistory.Reset();
		zipDepthActive = false;

		if (!vertex) {
			const auto compile = [](const char* entry, const char* target, ID3DBlob** code) {
				return D3DCompile(mfg::MotionShaders, sizeof(mfg::MotionShaders) - 1, nullptr, nullptr,
				                  nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, nullptr);
			};
			CComPtr<ID3DBlob> vs, lumaCode, vectorsCode;
			if (FAILED(compile("VS", "vs_5_0", &vs)) ||
			    FAILED(compile("Luma", "ps_5_0", &lumaCode)) ||
			    FAILED(compile("Vectors", "ps_5_0", &vectorsCode)) ||
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
		CDlssOpticalFlow::Options options;
		// Balanced SR optical flow uses a 4x4 grid with cost and validation.
		options.gridSize = 4;
		options.bidirectional = true;
		options.cost = true;
		if (!flow.Init(device, context, width, height, options)) {
			options.cost = false;
			if (!flow.Init(device, context, width, height, options)) {
				failure = L"Inactive (NVIDIA Optical Flow unavailable)";
				return false;
			}
		}
		if (flow.Bidirectional()) {
			// Balanced 4x4: reconstruct input-resolution motion continuously
			// *inside* coherent regions. At boundaries retain the center-cell
			// vector, avoiding interpolation between unrelated moving objects.
			// A shader compile failure keeps the previously validated path.
			const auto createShader = [&](const char* source, size_t length,
				const char* entry, CComPtr<ID3D11PixelShader>& shader) -> bool {
				CComPtr<ID3DBlob> code;
				return SUCCEEDED(D3DCompile(source, length, nullptr, nullptr,
					nullptr, entry, "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
					0, &code, nullptr)) &&
					SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),
						code->GetBufferSize(), nullptr, &shader));
			};
			if (flow.GridSize() == 4 && createShader(sr_motion::RefineShaders,
				sizeof(sr_motion::RefineShaders) - 1, "VectorsValidatedSR", validatedVectors)) {
				refinedValidatedMotion = true;
				validateMotion = true;
			} else {
				validateMotion = createShader(mfg::MotionShaders,
					sizeof(mfg::MotionShaders) - 1, "VectorsValidated", validatedVectors);
			}
			if (validateMotion && flow.HasCost()) {
				if (refinedValidatedMotion && createShader(sr_motion::RefineShaders,
					sizeof(sr_motion::RefineShaders) - 1, "VectorsValidatedCostSR", costValidatedVectors)) {
					refinedCostMotion = true;
					costValidatedMotion = true;
				} else {
					costValidatedMotion = createShader(mfg::MotionShaders,
						sizeof(mfg::MotionShaders) - 1, "VectorsValidatedCost", costValidatedVectors);
				}
			}
		}

		// Do not pay the per-frame cost-output overhead when the additional
		// validation shader is unsupported or backward flow is unavailable.
		if (flow.HasCost() && !costValidatedMotion) {
			options.cost = false;
			if (!flow.Init(device, context, width, height, options)) {
				failure = L"Inactive (NVIDIA Optical Flow fallback unavailable)";
				return false;
			}
		}

		const double scale = std::max(double(outputWidth) / width, double(outputHeight) / height);
		const bool dlaa = width == outputWidth && height == outputHeight;
		const auto quality = dlaa ? NVSDK_NGX_PerfQuality_Value_DLAA
			: scale <= 1.55 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
			: scale <= 1.85 ? NVSDK_NGX_PerfQuality_Value_Balanced
			: scale <= 2.25 ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
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
		auto result = NVSDK_NGX_D3D11_CreateFeature(context, NVSDK_NGX_Feature_SuperSampling, parameters, &feature);
		if (NVSDK_NGX_FAILED(result) || !feature) {
			if (feature) {
				NVSDK_NGX_D3D11_ReleaseFeature(feature);
				feature = nullptr;
			}
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, basicFlags);
			result = NVSDK_NGX_D3D11_CreateFeature(context, NVSDK_NGX_Feature_SuperSampling, parameters, &feature);
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
		activeStatus += std::format(L", optical flow {}x{}", flow.GridSize(), flow.GridSize());
		if (validateMotion) {
			activeStatus += costValidatedMotion ? L", cost-aware motion" : L", validated motion";
			if (costValidatedMotion ? refinedCostMotion : refinedValidatedMotion)
				activeStatus += L", edge-aware grid";
		}
		if (autoExposure) {
			activeStatus += L", auto exposure";
		}
		if (sceneEnabled) {
			activeStatus += L", scene-cut guard";
		}
		if (outputWidth != requestedWidth || outputHeight != requestedHeight) {
			activeStatus += std::format(L", intermediate {}x{}", outputWidth, outputHeight);
		}
		return true;
	}

	bool Evaluate(REFERENCE_TIME pts, float frameTimeMs, CZipDepth* zipDepth, bool sourceFrame, ID3D11ShaderResourceView* cleanSource)
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
            sourceFrame ? zipDepth : nullptr);
		const bool reset = discontinuity || sceneCut;
		if (reset) {
            zipHistory.Reset();
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
					context->PSSetShader(costValidatedVectors, nullptr, 0);
				} else {
					context->PSSetShader(validatedVectors, nullptr, 0);
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
		zipDepthActive = false;
		if (zipDepth) {
			const RECT picture{0, 0, LONG(w), LONG(h)};
			const SIZE modelSize{LONG(w), LONG(h)};
			if (sourceFrame && !cleanSource) {
				zipDepth->Update(device, context, input.pShaderResource, picture, modelSize, pts);
			} else {
				zipDepth->PollCompleted();
			}
			if (!reset && hasMotion) {
				zipDepthActive = zipHistory.Draw(device, context, *zipDepth, depth.pTexture, depthView,
					motion.pShaderResource, picture, modelSize, pts, frameTimeMs, false);
			}
		}
		if (!zipDepth || !hasMotion) zipHistory.Reset();
		if (zipDepthActive) {
			depthIsFlat = false;
		} else if (!depthIsFlat) {
			const FLOAT flatDepth[4] = {0.5f, 0, 0, 0};
			context->ClearRenderTargetView(depthView, flatDepth);
			depthIsFlat = true;
		}
		SubmitSceneThumbnail(pts, cleanSource ? cleanSource : input.pShaderResource.p);
		parameters->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(input.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D11Resource*>(motion.pTexture.p));
		parameters->Set(NVSDK_NGX_Parameter_Reset, int(reset || !hasMotion));
		parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
		parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, w);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, h);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
		parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, frameTimeMs);
		PollGpuTiming();
		GpuSample* gpuSample = BeginGpuTiming();
		const auto result = NVSDK_NGX_D3D11_EvaluateFeature(context, feature, parameters, nullptr);
		EndGpuTiming(gpuSample);
		if (NVSDK_NGX_FAILED(result)) {
			failure = std::format(L"Inactive (NGX evaluate {:#x})", unsigned(result));
			return false;
		}
		if (zipDepthActive) zipDepth->RecordSrEvaluation();
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

CDlssSR::CDlssSR() = default;
CDlssSR::~CDlssSR() = default;

Tex2D_t* CDlssSR::Prepare(ID3D11Device1* device, UINT width, UINT height, UINT outputWidth, UINT outputHeight)
{
	if (m_unavailable) {
		return nullptr;
	}
	if (!m_session) {
		m_session = std::make_unique<Session>();
		if (!m_session->Open(device)) {
			m_failure = m_session->failure;
			m_session.reset();
			m_unavailable = true;
			return nullptr;
		}
	}
	auto& session = *m_session;
	if (session.requestedInputWidth != width || session.requestedInputHeight != height ||
	    session.requestedWidth != outputWidth || session.requestedHeight != outputHeight) {
		session.sizeFailed = !session.CreateForOutput(width, height, outputWidth, outputHeight);
		m_failure = session.sizeFailed ? session.failure : std::wstring{};
	}
	return session.sizeFailed ? nullptr : &session.input;
}

Tex2D_t* CDlssSR::Evaluate(int64_t pts, float frameTimeMs, CZipDepth* zipDepth, bool sourceFrame, ID3D11ShaderResourceView* cleanSource)
{
	if (!m_session->Evaluate(pts, frameTimeMs, zipDepth, sourceFrame, cleanSource)) {
		if (m_session->TrySmallerOutput(m_session->input.desc.Width, m_session->input.desc.Height)) {
			return nullptr; // Use ordinary scaling for this frame while the new history starts.
		}
		m_failure = m_session->failure;
		m_session->validOutput = false;
		m_session->sizeFailed = true;
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
        m_session->zipHistory.Reset();
        m_session->zipDepthActive = false;
	}
}

void CDlssSR::Reset()
{
	m_session.reset();
	m_unavailable = false;
	m_failure.clear();
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

const wchar_t* CDlssSR::Status(const CZipDepth* zipDepth) const
{
	if (Active()) {
        if (zipDepth) {
            m_statusDetails = m_session->activeStatus + L", ZipDepth DirectML";
            if (m_session->zipDepthActive) {
                m_statusDetails += zipDepth->ForFrame(m_session->lastPts).view
                    ? L", estimated depth active" : L", reprojecting depth";
            } else {
                m_statusDetails += std::format(L", flat ({})", zipDepth->Status());
            }
            return m_statusDetails.c_str();
        }
		return m_session->activeStatus.c_str();
	}
	return m_failure.empty() ? L"Inactive (waiting for an eligible SDR upscale)" : m_failure.c_str();
}
