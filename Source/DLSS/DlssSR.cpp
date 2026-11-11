// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "DlssSR.h"
#include "DlssOpticalFlow.h"
#include "NgxRuntime.h"
#include "Mfg/MotionShaders.h"
#include "Helper.h"
#include "DX11Helper.h"
#include <d3dcompiler.h>
#include <filesystem>
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

} // namespace

struct CDlssSR::Session
{
	CComPtr<ID3D11Device1> device;
	CComPtr<ID3D11DeviceContext1> context;
	CComPtr<ID3DDeviceContextState> state;
	CComPtr<ID3D11VertexShader> vertex;
	CComPtr<ID3D11PixelShader> luma, vectors;
	CComPtr<ID3D11Buffer> constants;
	CComPtr<ID3D11RenderTargetView> depthView, motionView;
	Tex2D_t input, output, depth, motion;
	CDlssOpticalFlow flow;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	REFERENCE_TIME lastPts = -1;
	bool initialized = false;
	bool validOutput = false;
	std::wstring failure;

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
		const auto directory = std::filesystem::path(NgxRuntime::ModuleDir()) / L"dlss";
		std::error_code error;
		if (!std::filesystem::is_regular_file(directory / L"nvngx_dlss.dll", error)) {
			failure = L"Inactive (missing dlss/nvngx_dlss.dll)";
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

	bool Create(UINT width, UINT height, UINT outputWidth, UINT outputHeight)
	{
		failure = L"Inactive (DLSS SR texture or shader setup failed)";
		ContextStateScope scope(context, state);
		if (feature) {
			NVSDK_NGX_D3D11_ReleaseFeature(feature);
			feature = nullptr;
		}
		depthView.Release();
		motionView.Release();
		lastPts = -1;
		validOutput = false;
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
		options.bidirectional = false;
		options.cost = false;
		if (!flow.Init(device, context, width, height, options)) {
			failure = L"Inactive (NVIDIA Optical Flow unavailable)";
			return false;
		}

		const double scale = std::max(double(outputWidth) / width, double(outputHeight) / height);
		const auto quality = scale <= 1.55 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
			: scale <= 1.85 ? NVSDK_NGX_PerfQuality_Value_Balanced
			: scale <= 2.25 ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		parameters->Set(NVSDK_NGX_Parameter_Width, width);
		parameters->Set(NVSDK_NGX_Parameter_Height, height);
		parameters->Set(NVSDK_NGX_Parameter_OutWidth, outputWidth);
		parameters->Set(NVSDK_NGX_Parameter_OutHeight, outputHeight);
		parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, int(quality));
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
		const auto result = NVSDK_NGX_D3D11_CreateFeature(context, NVSDK_NGX_Feature_SuperSampling, parameters, &feature);
		if (NVSDK_NGX_FAILED(result) || !feature) {
			failure = std::format(L"Inactive (NGX create {:#x})", unsigned(result));
			return false;
		}
		return true;
	}

	bool Evaluate(REFERENCE_TIME pts, float frameTimeMs)
	{
		if (validOutput && pts >= 0 && pts == lastPts) {
			return true;
		}
		const bool reset = !validOutput || pts < 0 || lastPts < 0 || pts < lastPts || pts - lastPts > 2500000;
		if (reset) {
			flow.Reset();
		}
		ContextStateScope scope(context, state);
		context->ClearState();
		const UINT w = input.desc.Width, h = input.desc.Height;
		const D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertex, nullptr, 0);
		context->PSSetShader(luma, nullptr, 0);
		context->PSSetShaderResources(0, 1, &input.pShaderResource.p);
		auto* target = flow.FrameTarget();
		context->OMSetRenderTargets(1, &target, nullptr);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* empty[2] = {};
		context->PSSetShaderResources(0, 2, empty);
		const bool hasMotion = flow.Execute();
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
			context->PSSetShader(vectors, nullptr, 0);
			context->OMSetRenderTargets(1, &motionView.p, nullptr);
			context->Draw(3, 0);
		} else {
			const FLOAT zero[4] = {};
			context->ClearRenderTargetView(motionView, zero);
		}
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->PSSetShaderResources(0, 2, empty);
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
		const auto result = NVSDK_NGX_D3D11_EvaluateFeature(context, feature, parameters, nullptr);
		if (NVSDK_NGX_FAILED(result)) {
			failure = std::format(L"Inactive (NGX evaluate {:#x})", unsigned(result));
			return false;
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
	if ((!session.feature || session.input.desc.Width != width || session.input.desc.Height != height ||
	     session.output.desc.Width != outputWidth || session.output.desc.Height != outputHeight) &&
	    !session.Create(width, height, outputWidth, outputHeight)) {
		m_failure = session.failure;
		m_session.reset();
		m_unavailable = true;
		return nullptr;
	}
	return &session.input;
}

Tex2D_t* CDlssSR::Evaluate(int64_t pts, float frameTimeMs)
{
	if (!m_session->Evaluate(pts, frameTimeMs)) {
		m_failure = m_session->failure;
		m_session.reset();
		m_unavailable = true;
		return nullptr;
	}
	return &m_session->output;
}

void CDlssSR::ResetHistory()
{
	if (m_session) {
		m_session->validOutput = false;
		m_session->flow.Reset();
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

const wchar_t* CDlssSR::Status() const
{
	if (Active()) {
		return L"Active";
	}
	return m_failure.empty() ? L"Inactive (waiting for an eligible SDR upscale)" : m_failure.c_str();
}
