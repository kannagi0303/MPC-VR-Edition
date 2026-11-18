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

#include "stdafx.h"
#include "Mfg/SettingsScope.h"
#include "Mfg/DisplayCadence.h"
#include <uuids.h>
#include <Mferror.h>
#include <Mfidl.h>
#include <cmath>
#include <optional>
#include <limits>
#include "Helper.h"
#include "Times.h"
#include "Mfg/CadenceTrace.h"
#include "resource.h"
#include "VideoRenderer.h"
#include "Version.h"
#include "DX11VideoProcessor.h"
#include <ID3DVideoMemoryConfiguration.h>
#include "Shaders.h"
#include "Utils/CPUInfo.h"

#include <dxgi1_6.h>

#include "../external/minhook/include/MinHook.h"

bool g_bPresent = false;
bool g_bCreateSwapChain = false;
HWND g_hWnd = nullptr;

typedef BOOL(WINAPI* pSetWindowPos)(
	_In_ HWND hWnd,
	_In_opt_ HWND hWndInsertAfter,
	_In_ int X,
	_In_ int Y,
	_In_ int cx,
	_In_ int cy,
	_In_ UINT uFlags);

pSetWindowPos pOrigSetWindowPosDX11 = nullptr;
static BOOL WINAPI pNewSetWindowPosDX11(
	_In_ HWND hWnd,
	_In_opt_ HWND hWndInsertAfter,
	_In_ int X,
	_In_ int Y,
	_In_ int cx,
	_In_ int cy,
	_In_ UINT uFlags)
{
	if (g_bPresent && g_hWnd == hWnd) {
		DLog(L"call SetWindowPos() function during Present()");
		uFlags |= SWP_ASYNCWINDOWPOS;
	}
	return pOrigSetWindowPosDX11(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

typedef LONG(WINAPI* pSetWindowLongA)(
	_In_ HWND hWnd,
	_In_ int nIndex,
	_In_ LONG dwNewLong);

pSetWindowLongA pOrigSetWindowLongADX11 = nullptr;
static LONG WINAPI pNewSetWindowLongADX11(
	_In_ HWND hWnd,
	_In_ int nIndex,
	_In_ LONG dwNewLong)
{
	if (g_bCreateSwapChain && g_hWnd == hWnd) {
		DLog(L"Blocking call SetWindowLongA() function during create fullscreen swap chain");
		return 0L;
	}
	return pOrigSetWindowLongADX11(hWnd, nIndex, dwNewLong);
}

template <typename T>
inline bool HookFunc(T** ppSystemFunction, PVOID pHookFunction)
{
	return MH_CreateHook(*ppSystemFunction, pHookFunction, reinterpret_cast<LPVOID*>(ppSystemFunction)) == MH_OK;
}

static const ScalingShaderResId s_Upscaling11ResIDs[UPSCALE_COUNT] = {
	{0,                            0,                            L"Nearest-neighbor"  },
	{IDF_PS_11_INTERP_MITCHELL4_X, IDF_PS_11_INTERP_MITCHELL4_Y, L"Mitchell-Netravali"},
	{IDF_PS_11_INTERP_CATMULL4_X,  IDF_PS_11_INTERP_CATMULL4_Y,  L"Catmull-Rom"       },
	{IDF_PS_11_INTERP_LANCZOS2_X,  IDF_PS_11_INTERP_LANCZOS2_Y,  L"Lanczos2"          },
	{IDF_PS_11_INTERP_LANCZOS3_X,  IDF_PS_11_INTERP_LANCZOS3_Y,  L"Lanczos3"          },
	{IDF_PS_11_INTERP_JINC2,       IDF_PS_11_INTERP_JINC2,       L"Jinc2m"            },
};

static const ScalingShaderResId s_Downscaling11ResIDs[DOWNSCALE_COUNT] = {
	{IDF_PS_11_CONVOL_BOX_X,       IDF_PS_11_CONVOL_BOX_Y,       L"Box"          },
	{IDF_PS_11_CONVOL_BILINEAR_X,  IDF_PS_11_CONVOL_BILINEAR_Y,  L"Bilinear"     },
	{IDF_PS_11_CONVOL_HAMMING_X,   IDF_PS_11_CONVOL_HAMMING_Y,   L"Hamming"      },
	{IDF_PS_11_CONVOL_BICUBIC05_X, IDF_PS_11_CONVOL_BICUBIC05_Y, L"Bicubic"      },
	{IDF_PS_11_CONVOL_BICUBIC15_X, IDF_PS_11_CONVOL_BICUBIC15_Y, L"Bicubic sharp"},
	{IDF_PS_11_CONVOL_LANCZOS_X,   IDF_PS_11_CONVOL_LANCZOS_Y,   L"Lanczos"      }
};

const UINT dither_size = 32;

struct VERTEX {
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT2 TexCoord;
};

struct DEPTH3D_VERTEX {
	DirectX::XMFLOAT2 Pos;
	DirectX::XMFLOAT2 TexCoord;
	DirectX::XMFLOAT2 DepthCoord;
};

struct PS_EXTSHADER_CONSTANTS {
	DirectX::XMFLOAT2 pxy; // pixel size in normalized coordinates
	DirectX::XMFLOAT2 wh;  // width and height of texture
	uint32_t counter;      // rendered frame counter
	float clock;           // some time in seconds
	float reserved1;
	float reserved2;
};

static_assert(sizeof(PS_EXTSHADER_CONSTANTS) % 16 == 0);

static void FillVertices(VERTEX (&Vertices)[4], const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	const float src_dx = 1.0f / srcW;
	const float src_dy = 1.0f / srcH;
	float src_l = src_dx * srcRect.left;
	float src_r = src_dx * srcRect.right;
	const float src_t = src_dy * srcRect.top;
	const float src_b = src_dy * srcRect.bottom;

	POINT points[4];
	switch (iRotation) {
	case 90:
		points[0] = { -1, +1 };
		points[1] = { +1, +1 };
		points[2] = { -1, -1 };
		points[3] = { +1, -1 };
		break;
	case 180:
		points[0] = { +1, +1 };
		points[1] = { +1, -1 };
		points[2] = { -1, +1 };
		points[3] = { -1, -1 };
		break;
	case 270:
		points[0] = { +1, -1 };
		points[1] = { -1, -1 };
		points[2] = { +1, +1 };
		points[3] = { -1, +1 };
		break;
	default:
		points[0] = { -1, -1 };
		points[1] = { -1, +1 };
		points[2] = { +1, -1 };
		points[3] = { +1, +1 };
	}

	if (bFlip) {
		std::swap(src_l, src_r);
	}

	// Vertices for drawing whole texture
	// 2 ___4
	//  |\ |
	// 1|_\|3
	Vertices[0] = { {(float)points[0].x, (float)points[0].y, 0}, {src_l, src_b} };
	Vertices[1] = { {(float)points[1].x, (float)points[1].y, 0}, {src_l, src_t} };
	Vertices[2] = { {(float)points[2].x, (float)points[2].y, 0}, {src_r, src_b} };
	Vertices[3] = { {(float)points[3].x, (float)points[3].y, 0}, {src_r, src_t} };
}

static HRESULT CreateVertexBuffer(ID3D11Device* pDevice, ID3D11Buffer** ppVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	ASSERT(ppVertexBuffer);
	ASSERT(*ppVertexBuffer == nullptr);

	VERTEX Vertices[4];
	FillVertices(Vertices, srcW, srcH, srcRect, iRotation, bFlip);

	D3D11_BUFFER_DESC BufferDesc = { sizeof(Vertices), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA InitData = { Vertices, 0, 0 };

	HRESULT hr = pDevice->CreateBuffer(&BufferDesc, &InitData, ppVertexBuffer);
	DLogIf(FAILED(hr), L"CreateVertexBuffer() : CreateBuffer() failed with error {}", HR2Str(hr));

	return hr;
}

static HRESULT FillVertexBuffer(ID3D11DeviceContext* pDeviceContext, ID3D11Buffer* pVertexBuffer,
	const UINT srcW, const UINT srcH, const RECT& srcRect,
	const int iRotation, const bool bFlip)
{
	ASSERT(pVertexBuffer);

	VERTEX Vertices[4];
	FillVertices(Vertices, srcW, srcH, srcRect, iRotation, bFlip);

	D3D11_MAPPED_SUBRESOURCE mr;
	HRESULT hr = pDeviceContext->Map(pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"FillVertexBuffer() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &Vertices, sizeof(Vertices));
	pDeviceContext->Unmap(pVertexBuffer, 0);

	return hr;
}

static void TextureBlt11(
	ID3D11DeviceContext* pDeviceContext,
	ID3D11RenderTargetView* pRenderTargetView, D3D11_VIEWPORT& viewport,
	ID3D11InputLayout* pInputLayout,
	ID3D11VertexShader* pVertexShader,
	ID3D11PixelShader* pPixelShader,
	ID3D11ShaderResourceView* pShaderResourceViews,
	ID3D11SamplerState* pSampler,
	ID3D11Buffer* pConstantBuffer,
	ID3D11Buffer* pVertexBuffer,
	ID3D11BlendState* pBlendState = nullptr,
	const FLOAT* pBlendFactor = nullptr)
{
	ASSERT(pDeviceContext);
	ASSERT(pRenderTargetView);
	ASSERT(pShaderResourceViews);

	const UINT Stride = sizeof(VERTEX);
	const UINT Offset = 0;

	// Set resources
	pDeviceContext->IASetInputLayout(pInputLayout);
	pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
	pDeviceContext->RSSetViewports(1, &viewport);
	pDeviceContext->OMSetBlendState(pBlendState, pBlendFactor, D3D11_DEFAULT_SAMPLE_MASK);
	pDeviceContext->VSSetShader(pVertexShader, nullptr, 0);
	pDeviceContext->PSSetShader(pPixelShader, nullptr, 0);
	pDeviceContext->PSSetShaderResources(0, 1, &pShaderResourceViews);
	pDeviceContext->PSSetSamplers(0, 1, &pSampler);
	pDeviceContext->PSSetConstantBuffers(0, 1, &pConstantBuffer);
	pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	pDeviceContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	pDeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* views[1] = {};
	pDeviceContext->PSSetShaderResources(0, 1, views);
}

//
// CDX11VideoProcessor
//

HRESULT CDX11VideoProcessor::AlphaBlt(
	ID3D11ShaderResourceView* pShaderResource,
	ID3D11Texture2D* pRenderTarget,
	ID3D11Buffer* pVertexBuffer,
	D3D11_VIEWPORT* pViewPort,
	ID3D11SamplerState* pSampler)
{
	ID3D11RenderTargetView* pRenderTargetView;
	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);

	if (S_OK == hr) {
		UINT Stride = sizeof(VERTEX);
		UINT Offset = 0;

		// Set resources
		m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
		m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
		m_pDeviceContext->RSSetViewports(1, pViewPort);
		m_pDeviceContext->OMSetBlendState(m_pAlphaBlendState, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
		m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
		m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);
		m_pDeviceContext->PSSetShaderResources(0, 1, &pShaderResource);
		m_pDeviceContext->PSSetSamplers(0, 1, &pSampler);
		m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_pDeviceContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &Stride, &Offset);

		// Draw textured quad onto render target
		m_pDeviceContext->Draw(4, 0);

		pRenderTargetView->Release();
	}
	DLogIf(FAILED(hr), L"AlphaBlt() : CreateRenderTargetView() failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT CDX11VideoProcessor::TextureCopyRect(
	const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& destRect,
	ID3D11PixelShader* pPixelShader, ID3D11Buffer* pConstantBuffer,
	const int iRotation, const bool bFlip, const bool bLinear)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"TextureCopyRect() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, iRotation, bFlip);
	if (FAILED(hr)) {
		return hr;
	}

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)destRect.left;
	VP.TopLeftY = (FLOAT)destRect.top;
	VP.Width    = (FLOAT)destRect.Width();
	VP.Height   = (FLOAT)destRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, pPixelShader, Tex.pShaderResource, bLinear ? m_pSamplerLinear : m_pSamplerPoint, pConstantBuffer, m_pVertexBuffer);

	return hr;
}

HRESULT CDX11VideoProcessor::TextureResizeShader(
	const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& dstRect,
	ID3D11PixelShader* pPixelShader,
	const int iRotation, const bool bFlip)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::TextureResizeShader() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, iRotation, bFlip);
	if (FAILED(hr)) {
		return hr;
	}

	const FLOAT constants[][4] = {
		{(float)Tex.desc.Width, (float)Tex.desc.Height, 1.0f / Tex.desc.Width, 1.0f / Tex.desc.Height},
		{(float)srcRect.Width() / dstRect.Width(), (float)srcRect.Height() / dstRect.Height(), 0, 0}
	};

	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pResizeShaderConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::TextureResizeShader() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &constants, sizeof(constants));
	m_pDeviceContext->Unmap(m_pResizeShaderConstantBuffer, 0);

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)dstRect.left;
	VP.TopLeftY = (FLOAT)dstRect.top;
	VP.Width = (FLOAT)dstRect.Width();
	VP.Height = (FLOAT)dstRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, pPixelShader, Tex.pShaderResource, m_pSamplerPoint, m_pResizeShaderConstantBuffer, m_pVertexBuffer);

	return hr;
}

// CDX11VideoProcessor

static void ApplyJvrPlaybackMode(Settings_t& config, bool projectJvr)
{
	if (!projectJvr) return;

	// The decoder VP must deliver source colour for projection. Its RTX
	// enhancements would operate on both eyes before the RGB source exists.
	config.iVPSuperRes = SUPERRES_Disable;
	config.bVPRTXVideoHDR = false;
}

CDX11VideoProcessor::CDX11VideoProcessor(CMpcVideoRenderer* pFilter, const Settings_t& requested, HRESULT& hr)
	: CVideoProcessor(pFilter)
{
	Settings_t config = requested;
	mfg::EnforceScope(config);
	m_jvrFlatView = m_pFilter->JvrProjectionRequested(config);
	ApplyJvrPlaybackMode(config, m_jvrFlatView);
	ConfigureJvrProjection(config);
	m_mfgMultiplier=(config.bUseD3D11 ? config.iMfgMultiplier : 0);m_rtVideoDelay=config.iVideoDelay*10000LL;
    m_mfgProcessingMode = config.iMfgProcessingMode <= 1 ? config.iMfgProcessingMode : 0;
	m_svdLut.strength = config.GetSvdLutStrength();
	m_useZipDepthSr = config.bUseZipDepthSr;
	m_useZipDepthMfg = config.bUseZipDepthMfg;
	m_adaptiveMfgDepth = config.bAdaptiveMfgDepth;
	m_showMfgMotion = config.bShowMfgMotion;
	m_showDepthOverlay = config.bShowDepthOverlay;
	m_depth3DPreview = config.bDepth3DPreview;
	m_mfgLowMotionEnabled = config.bMfgLowMotion;
	m_bShowStats           = config.bShowStats;
	m_showFeatureStatus.store(config.bShowFeatureBadges, std::memory_order_relaxed);
	m_iResizeStats         = config.iResizeStats;
	m_iTexFormat           = config.iTexFormat;
	m_VPFormats            = config.VPFmts;
	m_iVPDeinterlacing     = config.iVPDeinterlacing;
	m_bDeintDouble         = config.bDeintDouble;
	m_bVPScaling           = config.bVPScaling;
	m_iVPSuperRes          = config.bDlssSR ? SUPERRES_DLSS_SR : config.iVPSuperRes;
	m_dlssSRSourceLimit    = config.iDlssSRSourceLimit;
	m_dlssSRDlaa          = config.bDlssSRDlaa;
	m_bVPRTXVideoHDR       = config.bVPRTXVideoHDR;
	m_iChromaScaling       = config.iChromaScaling;
	m_iUpscaling           = config.iUpscaling;
	m_iDownscaling         = config.iDownscaling;
	m_bInterpolateAt50pct  = config.bInterpolateAt50pct;
	m_bUseDither           = config.bUseDither;
	m_bDeintBlend          = config.bDeintBlend;
	m_iSwapEffect          = config.iSwapEffect;
	m_bVBlankBeforePresent = config.bVBlankBeforePresent;
	m_bAdjustPresentTime   = config.bAdjustPresentTime;
	m_bHdrPreferDoVi       = config.bHdrPreferDoVi;
	m_bHdrPassthrough      = config.bHdrPassthrough;
	m_bHdrLocalToneMapping = config.bHdrLocalToneMapping;
	m_iHdrLocalToneMappingType = config.iHdrLocalToneMappingType;
	m_bHdrLocalMeasurePeak = config.bHdrLocalMeasurePeak;
	m_iHdrDoviToneMappingType = config.iHdrDoviToneMappingType;
	m_iHdrDisplayMaxNits   = config.iHdrDisplayMaxNits;
	m_iHdrToggleDisplay    = config.iHdrToggleDisplay;
	m_iHdrOsdBrightness    = config.iHdrOsdBrightness;
	m_bConvertToSdr        = config.bConvertToSdr;
	m_bSdrToneMapping      = config.iSdrToneMappingType != SDRTM_Fixed;
	m_bSdrMeasurePeak      = config.iSdrToneMappingType == SDRTM_Measured;
	m_iSdrPeakWindowMs     = config.iSdrPeakWindowMs;
	m_iSdrPeakFloorNits    = config.iSdrPeakFloorNits;
	m_bSdrPeakSceneCuts    = config.bSdrPeakSceneCuts;
	m_iSDRDisplayNits      = config.iSDRDisplayNits;

	m_nCurrentAdapter = -1;

	hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&m_pDXGIFactory1);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::CDX11VideoProcessor() : CreateDXGIFactory1() failed with error {}", HR2Str(hr));
		return;
	}

	// set default ProcAmp ranges and values
	SetDefaultDXVA2ProcAmpRanges(m_DXVA2ProcAmpRanges);
	SetDefaultDXVA2ProcAmpValues(m_DXVA2ProcAmpValues);

	pOrigSetWindowPosDX11 = SetWindowPos;
	auto ret = HookFunc(&pOrigSetWindowPosDX11, pNewSetWindowPosDX11);
	DLogIf(!ret, L"CDX11VideoProcessor::CDX11VideoProcessor() : hook for SetWindowPos() fail");

	pOrigSetWindowLongADX11 = SetWindowLongA;
	ret = HookFunc(&pOrigSetWindowLongADX11, pNewSetWindowLongADX11);
	DLogIf(!ret, L"CDX11VideoProcessor::CDX11VideoProcessor() : hook for SetWindowLongA() fail");

	MH_EnableHook(MH_ALL_HOOKS);

	CComPtr<IDXGIAdapter> pDXGIAdapter;
	for (UINT adapter = 0; m_pDXGIFactory1->EnumAdapters(adapter, &pDXGIAdapter) != DXGI_ERROR_NOT_FOUND; ++adapter) {
		CComPtr<IDXGIOutput> pDXGIOutput;
		for (UINT output = 0; pDXGIAdapter->EnumOutputs(output, &pDXGIOutput) != DXGI_ERROR_NOT_FOUND; ++output) {
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(pDXGIOutput->GetDesc(&desc))) {
				DisplayConfig_t displayConfig = {};
				if (GetDisplayConfig(desc.DeviceName, displayConfig)) {
					m_hdrModeStartState[desc.DeviceName] = displayConfig.HDREnabled();
				}
			}

			pDXGIOutput.Release();
		}

		pDXGIAdapter.Release();
	}
}

CDX11VideoProcessor::~CDX11VideoProcessor()
{
	m_fastShutdown = true;
	m_completedPresenter.Stop();
	for (const auto& [displayName, state] : m_hdrModeSavedState) {
		DisplayConfig_t displayConfig = {};
		if (GetDisplayConfig(displayName.c_str(), displayConfig)) {
			if (displayConfig.HDRSupported() && displayConfig.HDREnabled() != state) {
				const auto ret = ToggleHDR(displayConfig, state);
				DLogIf(!ret, L"CDX11VideoProcessor::~CDX11VideoProcessor() : Toggle HDR {} for '{}' failed", state ? L"ON" : L"OFF", displayName);
			}
		}
	}

	m_pFilter->m_pSubPicQueue.Release();
	m_pSubPicAllocator.Release();

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();

	ReleaseDevice();

	m_pDXGIFactory1.Release();

	MH_RemoveHook(SetWindowPos);
	MH_RemoveHook(SetWindowLongA);
}

void CDX11VideoProcessor::FillDisplayParams()
{
	m_bHdrPassthroughSupport = false;
	m_bHdrDisplayModeEnabled = false;
	m_DisplayBitsPerChannel = 8;

	m_bACMEnabled = false;

	MONITORINFOEXW mi = { sizeof(mi) };
	GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), reinterpret_cast<LPMONITORINFO>(&mi));
	DisplayConfig_t displayConfig = {};

	if (GetDisplayConfig(mi.szDevice, displayConfig)) {
		m_bHdrDisplayModeEnabled = displayConfig.HDREnabled();
		m_bHdrPassthroughSupport = displayConfig.HDRSupported() && m_bHdrDisplayModeEnabled;
		m_DisplayBitsPerChannel = displayConfig.bitsPerChannel;

		m_bACMEnabled = !m_bHdrDisplayModeEnabled && displayConfig.ACMEnabled();

#ifdef _DEBUG
		auto displayDesc = std::format(L"\nDisplay: {} {}", mi.szDevice, DisplayConfigToString(displayConfig));
		if (displayConfig.bitsPerChannel) {
			auto colenc = ColorEncodingToString(displayConfig.colorEncoding);
			if (colenc) {
				displayDesc.append(std::format(L"\n  Color: {} {}-bit", colenc, displayConfig.bitsPerChannel));
				if (displayConfig.HDRSupported()) {
					displayDesc.append(L", HDR10: ");
					displayDesc.append(displayConfig.HDREnabled() ? L"on" : L"off");

					if (IsWindows11_24H2OrGreater()) {
						auto ColorModeToStr = [](DISPLAYCONFIG_ADVANCED_COLOR_MODE ColorMode) {
							std::wstring str;
#define UNPACK_VALUE(VALUE) case VALUE: str = L"" #VALUE; break;
							switch (ColorMode) {
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR);
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG);
								UNPACK_VALUE(DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR);
								default:
									str = std::to_wstring(static_cast<int>(ColorMode));
							};
#undef UNPACK_VALUE

							return str;
						};

						const auto& colors = displayConfig.windows1124H2Colors;
						displayDesc.append(std::format(L"\n         Advanced Color: Supported: {}, Active: {}, Limited by OS policy: {}, HDR is supported: {}",
													   colors.advancedColorSupported, colors.advancedColorActive,
													   colors.advancedColorLimitedByPolicy, colors.highDynamicRangeSupported));
						displayDesc.append(std::format(L"\n                         HDR enabled: {}, Wide supported: {}, Wide enabled: {}",
													   colors.highDynamicRangeUserEnabled, colors.wideColorSupported,
													   colors.wideColorUserEnabled));
						displayDesc.append(std::format(L"\n                         Display color mode: {}", ColorModeToStr(colors.activeColorMode)));
					} else {
						const auto& colors = displayConfig.advancedColor;
						displayDesc.append(std::format(L"\n         Advanced Color: Supported: {}, Enabled: {}, Wide forced: {}, Force disabled: {}",
													   colors.advancedColorSupported, colors.advancedColorEnabled,
													   colors.wideColorEnforced, colors.advancedColorForceDisabled));
					}
				}
			}
		}

		DLog(L"CDX11VideoProcessor::FillDisplayParams():{}", displayDesc);
#endif
	}
}

HRESULT CDX11VideoProcessor::Init(const HWND hwnd, const bool displayHdrChanged, bool* pChangeDevice/* = nullptr*/)
{
	DLog(L"CDX11VideoProcessor::Init()");

	const bool bWindowChanged = displayHdrChanged || (m_hWnd != hwnd);
	g_hWnd = m_hWnd = hwnd;

	FillDisplayParams();

	if (m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen) {
		m_srcVideoTransferFunction = 0;
	}

	if (!m_pDXGIFactory1->IsCurrent()) {
		m_pDXGIFactory1.Release();

		auto hr = CreateDXGIFactory1(IID_IDXGIFactory1, (void**)&m_pDXGIFactory1);
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::Init() : CreateDXGIFactory1() failed with error {}", HR2Str(hr));
			return E_FAIL;
		}
	}

	IDXGIAdapter* pDXGIAdapter = nullptr;
	const UINT currentAdapter = GetAdapter(hwnd, m_pDXGIFactory1, &pDXGIAdapter);
	CheckPointer(pDXGIAdapter, E_FAIL);
	if (m_nCurrentAdapter == currentAdapter) {
		SAFE_RELEASE(pDXGIAdapter);

		SetCallbackDevice();

		if (m_hWnd &&
				(!m_pDXGISwapChain1 || m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen || bWindowChanged)) {
			InitSwapChain(bWindowChanged);
			UpdateStatsStatic();
			m_pFilter->OnDisplayModeChange();
		}

		return S_OK;
	}
	m_nCurrentAdapter = currentAdapter;

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();
	ReleaseDevice();

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	D3D_FEATURE_LEVEL featurelevel;

	ID3D11Device *pDevice = nullptr;

	HRESULT hr = D3D11CreateDevice(
		pDXGIAdapter,
		D3D_DRIVER_TYPE_UNKNOWN,
		nullptr,
#ifdef _DEBUG
		D3D11_CREATE_DEVICE_DEBUG,
#else
		0,
#endif
		featureLevels,
		std::size(featureLevels),
		D3D11_SDK_VERSION,
		&pDevice,
		&featurelevel,
		nullptr);
#ifdef _DEBUG
	if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING || (hr == E_FAIL && !IsWindows8OrGreater())) {
		DLog(L"WARNING: D3D11 debugging messages will not be displayed");
		hr = D3D11CreateDevice(
			pDXGIAdapter,
			D3D_DRIVER_TYPE_UNKNOWN,
			nullptr,
			0,
			featureLevels,
			std::size(featureLevels),
			D3D11_SDK_VERSION,
			&pDevice,
			&featurelevel,
			nullptr);
	}
#endif
	SAFE_RELEASE(pDXGIAdapter);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::Init() : D3D11CreateDevice() failed with error {}", HR2Str(hr));
		return hr;
	}

	DLog(L"CDX11VideoProcessor::Init() : D3D11CreateDevice() successfully with feature level {}.{}", (featurelevel >> 12), (featurelevel >> 8) & 0xF);

	hr = SetDevice(pDevice, nullptr);
	pDevice->Release();

	if (S_OK == hr) {
		if (pChangeDevice) {
			*pChangeDevice = true;
		}
	}

	if (m_VendorId == PCIV_INTEL && CPUInfo::HaveSSE41()) {
		m_pCopyGpuFn = CopyGpuFrame_SSE41;
	} else {
		m_pCopyGpuFn = CopyPlaneAsIs;
	}

	return hr;
}

bool CDX11VideoProcessor::Initialized() const
{
	return (m_pDevice.p != nullptr && m_pDeviceContext.p != nullptr);
}

void CDX11VideoProcessor::ReleaseVP()
{
	m_zipDepth.ResetHistory();
	m_dlssSRAfterMfg = false;
	m_dlssSR.Reset();
	m_dlssSRSource.Release();
	m_depthOverlayCapture.Release();
    // A media-format change also invalidates the dedicated post-MFG VP.
    if(m_mfgPostVsrReady || m_mfgPostVsrBroken) StopMfg();
	DLog(L"CDX11VideoProcessor::ReleaseVP()");

	m_pFilter->ResetStreamingTimes2();
	m_RenderStats.Reset();

	if (m_pDeviceContext) {
		m_pDeviceContext->ClearState();
	}

	m_TexSrcVideo.Release();
	m_mfgNativeInput.Release();
	m_jvrSource.Release();
	m_jvrFrame.Release();
	m_TexConvertOutput.Release();
	m_TexResize.Release();
	m_TexsPostScale.Release();
	m_processingTexture.Release();
	m_processingPostScale.Release();
	m_processingSize = CSize(0, 0);

	m_PSConvColorData.Release();
	m_pDoviCurvesConstantBuffer.Release();

	m_D3D11VP.ReleaseVideoProcessor();
	m_strCorrection = nullptr;

	m_srcParams      = {};
	m_srcDXGIFormat  = DXGI_FORMAT_UNKNOWN;
	m_pCopyPlaneFn   = CopyPlaneAsIs;
	m_srcWidth       = 0;
	m_srcHeight      = 0;
}

void CDX11VideoProcessor::ReleaseDevice()
{
	ClearCompletedFrames(true);
	StopMfg(true, m_fastShutdown);
	m_zipDepth.Stop(m_fastShutdown);
	ReleaseSvdLut();
	DLog(L"CDX11VideoProcessor::ReleaseDevice()");

	ReleaseVP();
	m_D3D11VP.ReleaseVideoDevice();

	m_StatsBackground.InvalidateDeviceObjects();
	m_Font3D.InvalidateDeviceObjects();
	m_Rect3D.InvalidateDeviceObjects();
	m_FeatureStatusBackground.InvalidateDeviceObjects();

	m_Underlay.InvalidateDeviceObjects();
	m_Lines.InvalidateDeviceObjects();
	m_SyncLine.InvalidateDeviceObjects();

	m_TexDither.Release();
	m_bAlphaBitmapEnable = false;
	m_pAlphaBitmapVertex.Release();
	m_TexAlphaBitmap.Release();

	ClearPreScaleShaders();
	ClearPostScaleShaders();
	m_pPSCorrection.Release();
	ReleaseHdrMeasure();
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();

	m_pPSHDR10ToneMapping.Release();

	m_pShaderUpscaleX.Release();
	m_pShaderUpscaleY.Release();
	m_pShaderDownscaleX.Release();
	m_pShaderDownscaleY.Release();
	m_strShaderX = nullptr;
	m_strShaderY = nullptr;
	m_pPSFinalPass.Release();

	m_pCorrectionConstants.Release();
	m_pPostScaleConstants.Release();

	m_pHDR10ToneMappingConstants.Release();
	m_pDoViDynamicConstants.Release();

	m_pVSimpleInputLayout.Release();
	m_pVS_Simple.Release();
	m_pPS_Simple.Release();
	m_jvrShader.Release();
	m_jvrConstants.Release();
	m_jvrShaderFailed = false;
	m_pPS_BitmapToFrame.Release();
	m_pPS_DepthOverlay.Release();
	m_pDepthOverlayBlendState.Release();
	m_pDepthOverlayConstants.Release();
	m_pVS_Depth3DPreview.Release();
	m_pPS_Depth3DPreview.Release();
	m_pVDepth3DPreviewInputLayout.Release();
	m_pDepth3DPreviewConstants.Release();
	m_pDepth3DPreviewVertexBuffer.Release();
	m_pDepth3DPreviewIndexBuffer.Release();
	m_pDepth3DPreviewRasterizer.Release();
	m_depth3DPreviewIndexCount = 0;
	m_depth3DPreviewTexture.Release();
	m_pMfgMotionShader.Release();
	m_pMfgMotionBlend.Release();
	m_pMfgMotionConstants.Release();
	m_depthOverlayShaderFailed = false;
	m_depth3DPreviewShaderFailed = false;
	m_pSamplerPoint.Release();
	m_pSamplerLinear.Release();
	m_pSamplerDither.Release();
	m_pAlphaBlendState.Release();

	m_Alignment.texture.Release();
	m_Alignment.cformat = {};
	m_Alignment.cx = {};

	m_pVertexBuffer.Release();
	m_pResizeShaderConstantBuffer.Release();
	m_pHalfOUtoInterlaceConstantBuffer.Release();
	m_pFinalPassConstantBuffer.Release();

	if (m_pDeviceContext) {
		// need ClearState() (see ReleaseVP()) and Flush() for ID3D11DeviceContext when using DXGI_SWAP_EFFECT_DISCARD in Windows 8/8.1
		m_pDeviceContext->Flush();
	}
	m_pDeviceContext.Release();
	m_bCallbackDeviceIsSet = false;

#if (1 && _DEBUG)
	if (m_pDevice) {
		ID3D11Debug* pDebugDevice = nullptr;
		HRESULT hr2 = m_pDevice->QueryInterface(IID_PPV_ARGS(&pDebugDevice));
		if (S_OK == hr2) {
			hr2 = pDebugDevice->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
			ASSERT(S_OK == hr2);
		}
		SAFE_RELEASE(pDebugDevice);
	}
#endif

	m_pDevice.Release();
}

void CDX11VideoProcessor::ReleaseSwapChain()
{
    // A deferred window resize cannot outlive a swap-chain recreation.
    if (m_resizePending) {
        m_windowRect = m_resizeTarget;
        m_resizePending = false;
        UpdateRenderRect();
    }
	StopMfg(true, m_fastShutdown);
	if (m_pDXGISwapChain1) {
		m_pDXGISwapChain1->SetFullscreenState(FALSE, nullptr);
	}
	m_pDXGIOutput.Release();
	m_pDXGISwapChain4.Release();
	m_pDXGISwapChain1.Release();

	m_MaxDisplayLuminance = 0;
}

UINT CDX11VideoProcessor::GetPostScaleSteps()
{
	UINT nSteps = m_pPostScaleShaders.size();
	if (m_pPSCorrection) {
		nSteps++;
	}
	if (m_pPSHDR10ToneMapping) {
		nSteps++;
	}
	if (m_pPSHalfOUtoInterlace) {
		nSteps++;
	}
	if (m_bFinalPass) {
		nSteps++;
	}
	return nSteps;
}

HRESULT CDX11VideoProcessor::CreatePShaderFromResource(ID3D11PixelShader** ppPixelShader, UINT resid)
{
	if (!m_pDevice || !ppPixelShader) {
		return E_POINTER;
	}

	LPVOID data;
	DWORD size;
	HRESULT hr = GetDataFromResource(data, size, resid);
	if (FAILED(hr)) {
		return hr;
	}

	return m_pDevice->CreatePixelShader(data, size, nullptr, ppPixelShader);
}

void CDX11VideoProcessor::SetShaderConvertColorParams()
{
	mp_cmat cmatrix;

	if (m_Dovi.bValid) {
		const float brightness = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Brightness) / 255;
		const float contrast   = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Contrast);

		for (int i = 0; i < 3; i++) {
			cmatrix.m[i][0] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 0] * contrast;
			cmatrix.m[i][1] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 1] * contrast;
			cmatrix.m[i][2] = (float)m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix[i * 3 + 2] * contrast;
		}

		for (int i = 0; i < 3; i++) {
			cmatrix.c[i] = brightness;
			for (int j = 0; j < 3; j++) {
				cmatrix.c[i] -= cmatrix.m[i][j] * m_Dovi.msd.ColorMetadata.ycc_to_rgb_offset[j];
			}
		}

		m_PSConvColorData.bEnable = true;
	}
	else {
		mp_csp_params csp_params;
		set_colorspace(m_srcExFmt, csp_params.color);
		csp_params.brightness = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Brightness) / 255;
		csp_params.contrast   = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Contrast);
		csp_params.hue        = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Hue) / 180 * acos(-1);
		csp_params.saturation = DXVA2FixedToFloat(m_DXVA2ProcAmpValues.Saturation);
		csp_params.gray       = m_srcParams.CSType == CS_GRAY;

		csp_params.input_bits = csp_params.texture_bits = m_srcParams.CDepth;

		mp_get_csp_matrix(&csp_params, &cmatrix);

		m_PSConvColorData.bEnable =
			m_srcParams.CSType == CS_YUV ||
			m_srcParams.cformat == CF_GBRP8 || m_srcParams.cformat == CF_GBRP10 || m_srcParams.cformat == CF_GBRP16 ||
			csp_params.gray ||
			fabs(csp_params.brightness) > 1e-4f || fabs(csp_params.contrast - 1.0f) > 1e-4f;
	}

	PS_COLOR_TRANSFORM cbuffer = {
		{cmatrix.m[0][0], cmatrix.m[0][1], cmatrix.m[0][2], 0},
		{cmatrix.m[1][0], cmatrix.m[1][1], cmatrix.m[1][2], 0},
		{cmatrix.m[2][0], cmatrix.m[2][1], cmatrix.m[2][2], 0},
		{cmatrix.c[0],    cmatrix.c[1],    cmatrix.c[2],    0},
	};

	if (m_srcParams.cformat == CF_GBRP8 || m_srcParams.cformat == CF_GBRP10 || m_srcParams.cformat == CF_GBRP16) {
		std::swap(cbuffer.cm_r.x, cbuffer.cm_r.y); std::swap(cbuffer.cm_r.y, cbuffer.cm_r.z);
		std::swap(cbuffer.cm_g.x, cbuffer.cm_g.y); std::swap(cbuffer.cm_g.y, cbuffer.cm_g.z);
		std::swap(cbuffer.cm_b.x, cbuffer.cm_b.y); std::swap(cbuffer.cm_b.y, cbuffer.cm_b.z);
	}
	else if (m_srcParams.CSType == CS_GRAY) {
		cbuffer.cm_g.x = cbuffer.cm_g.y;
		cbuffer.cm_g.y = 0;
		cbuffer.cm_b.x = cbuffer.cm_b.z;
		cbuffer.cm_b.z = 0;
	}

	if (m_PSConvColorData.pConstants) {
		m_pDeviceContext->UpdateSubresource(m_PSConvColorData.pConstants, 0, nullptr, &cbuffer, 0, 0);
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_PSConvColorData.pConstants));
	}
}

// The peak that the HDR to SDR conversion maps onto the display's white: what the file claims,
// clamped to something usable.  0 means the Fixed method, which leaves the fixed curve in place.
float CDX11VideoProcessor::GetSdrToneMappingPeak() const
{
	if (!m_bSdrToneMapping) {
		return 0.0f;
	}
	const auto& meta = m_hdr10.bValid ? m_hdr10 : m_lastHdr10;
	return SdrToneMappingPeakNits(
		meta.bValid ? static_cast<float>(meta.hdr10.MaxContentLightLevel) : 0.0f,
		meta.bValid ? static_cast<float>(meta.hdr10.MaxMasteringLuminance) : 0.0f, // in nits, as CopySample stores it
		m_Dovi.bValid ? &m_Dovi.msd : nullptr,
		m_iSDRDisplayNits);
}

// Tuning of the per-frame peak measurement
constexpr UINT  kHdrHistBins       = 1024;    // must match HIST_BINS in cs_hdr_hist.hlsl and cs_hdr_resolve.hlsl
constexpr float kHdrPeakFraction   = 1.0e-4f; // share of pixels that may be brighter than the reported peak
constexpr float kHdrReleaseSeconds = 1.0f;    // time for the peak to relax towards a lower measurement, with no window
constexpr float kHdrSceneCutPQ     = 0.10f;   // a change larger than this (in PQ) is taken as a scene change
constexpr float kHdrNoSceneCutPQ   = 2.0f;    // more than any change in mean PQ can be: the window never restarts
constexpr UINT  kHdrStateHead      = 5;       // must match STATE_HEAD in cs_hdr_resolve.hlsl
constexpr UINT  kHdrWindowFrames   = 512;     // must match WINDOW_SIZE in cs_hdr_resolve.hlsl

bool CDX11VideoProcessor::HdrMeasureSupported() const
{
	return m_pDevice && m_pDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0;
}

HRESULT CDX11VideoProcessor::InitHdrMeasure()
{
	if (m_pHdrStateBuffer) {
		return S_OK;
	}
	if (!HdrMeasureSupported()) {
		return E_NOTIMPL;
	}

	auto CreateCS = [this](CComPtr<ID3D11ComputeShader>& pShader, UINT resid) {
		LPVOID data;
		DWORD size;
		HRESULT hr = GetDataFromResource(data, size, resid);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreateComputeShader(data, size, nullptr, &pShader);
		}
		return hr;
	};

	HRESULT hr = CreateCS(m_pCSHdrHist, IDF_CS_11_HDR_HIST);
	if (SUCCEEDED(hr)) {
		hr = CreateCS(m_pCSHdrResolve, IDF_CS_11_HDR_RESOLVE);
	}

	// the histogram, one counter per bin
	if (SUCCEEDED(hr)) {
		const UINT zeros[kHdrHistBins] = {};
		const D3D11_BUFFER_DESC desc = {
			.ByteWidth = sizeof(zeros),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = sizeof(UINT),
		};
		const D3D11_SUBRESOURCE_DATA init = { zeros, 0, 0 };
		hr = m_pDevice->CreateBuffer(&desc, &init, &m_pHdrHistBuffer);
	}
	if (SUCCEEDED(hr)) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kHdrHistBins;
		hr = m_pDevice->CreateUnorderedAccessView(m_pHdrHistBuffer, &uavDesc, &m_pHdrHistUAV);
	}

	// the smoothed result and the window of frame peaks (see the layout in cs_hdr_resolve.hlsl)
	if (SUCCEEDED(hr)) {
		const float zeros[(kHdrStateHead + kHdrWindowFrames) * 4] = {};
		const D3D11_BUFFER_DESC desc = {
			.ByteWidth = sizeof(zeros),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = 4 * sizeof(float),
		};
		const D3D11_SUBRESOURCE_DATA init = { zeros, 0, 0 };
		hr = m_pDevice->CreateBuffer(&desc, &init, &m_pHdrStateBuffer);
	}
	if (SUCCEEDED(hr)) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kHdrStateHead + kHdrWindowFrames;
		hr = m_pDevice->CreateUnorderedAccessView(m_pHdrStateBuffer, &uavDesc, &m_pHdrStateUAV);
	}
	if (SUCCEEDED(hr)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = 4;
		hr = m_pDevice->CreateShaderResourceView(m_pHdrStateBuffer, &srvDesc, &m_pHdrStateSRV);
	}

	// constants for the two passes
	if (SUCCEEDED(hr)) {
		D3D11_BUFFER_DESC desc = {
			.ByteWidth = sizeof(HdrMeasureConstants_t),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pHdrMeasureConstants);
		if (SUCCEEDED(hr)) {
			desc.ByteWidth = sizeof(HdrResolveConstants_t);
			hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_pHdrResolveConstants);
		}
	}

	// for the statistics only, so that a failure here is not fatal
	{
		const D3D11_BUFFER_DESC desc = {
			.ByteWidth = 64,
			.Usage = D3D11_USAGE_STAGING,
			.CPUAccessFlags = D3D11_CPU_ACCESS_READ,
		};
		m_pDevice->CreateBuffer(&desc, nullptr, &m_pHdrStateStaging);
	}

	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitHdrMeasure() : failed with error {}", HR2Str(hr));
		ReleaseHdrMeasure();
		return hr;
	}

	DLog(L"CDX11VideoProcessor::InitHdrMeasure() : ready");
	return S_OK;
}

void CDX11VideoProcessor::ReleaseHdrMeasure()
{
	m_bSdrMeasureActive = false;
	m_bHdrLocalMeasureActive = false;
	m_bHdrStatsCopyPending = false;
	m_fHdrMeasuredPeakNits = 0.0f;
	m_fHdrSmoothedPeakNits = 0.0f;

	m_pHdrStateStaging.Release();
	m_pHdrResolveConstants.Release();
	m_pHdrMeasureConstants.Release();
	m_pHdrStateSRV.Release();
	m_pHdrStateUAV.Release();
	m_pHdrStateBuffer.Release();
	m_pHdrHistUAV.Release();
	m_pHdrHistBuffer.Release();
	m_pCSHdrResolve.Release();
	m_pCSHdrHist.Release();
}

// Measures the frame the conversion is about to process and leaves the smoothed peak in
// m_pHdrStateBuffer, where the measuring variant of the conversion shader picks it up.
HRESULT CDX11VideoProcessor::MeasureHdrPeak(const Tex2D_t& tex, const CRect& rect)
{
	if (!(m_bSdrMeasureActive || m_bHdrLocalMeasureActive) || !m_pHdrStateBuffer || !tex.pShaderResource) {
		return E_ABORT;
	}

	CRect r;
	r.IntersectRect(rect, CRect(0, 0, tex.desc.Width, tex.desc.Height));
	if (r.IsRectEmpty()) {
		return S_FALSE;
	}

	float frameTime = m_rtAvgTimePerFrame > 0 ? static_cast<float>(m_rtAvgTimePerFrame) / 10000000.0f : 1.0f / 24.0f;
	if (m_bDeintDouble && m_bInterlaced && m_D3D11VP.IsReady()) {
		frameTime *= 0.5f; // two Render() calls per source frame
	}
	frameTime = std::clamp(frameTime, 1.0f / 120.0f, 0.1f);

	const HdrMeasureConstants_t measureConstants = {
		{ static_cast<UINT>(r.left), static_cast<UINT>(r.top) },
		{ static_cast<UINT>(r.Width()), static_cast<UINT>(r.Height()) }
	};
	// the peak is the mean of the frame peaks over the window, restarted at a scene change if
	// that is wanted.  madVR never restarts it, so off is what matches madVR.
	const HdrResolveConstants_t resolveConstants = {
		frameTime, kHdrReleaseSeconds, m_bSdrPeakSceneCuts ? kHdrSceneCutPQ : kHdrNoSceneCutPQ, kHdrPeakFraction,
		m_iSdrPeakWindowMs / 1000.0f, static_cast<float>(m_iSdrPeakFloorNits)
	};
	m_pDeviceContext->UpdateSubresource(m_pHdrMeasureConstants, 0, nullptr, &measureConstants, 0, 0);
	m_pDeviceContext->UpdateSubresource(m_pHdrResolveConstants, 0, nullptr, &resolveConstants, 0, 0);

	// the previous pass may have left this texture bound as the render target
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	// pass 1: histogram of the frame
	ID3D11ShaderResourceView* pSRV = tex.pShaderResource;
	ID3D11UnorderedAccessView* pUAVs[2] = { m_pHdrHistUAV, m_pHdrStateUAV };
	m_pDeviceContext->CSSetShader(m_pCSHdrHist, nullptr, 0);
	m_pDeviceContext->CSSetShaderResources(0, 1, &pSRV);
	m_pDeviceContext->CSSetConstantBuffers(0, 1, &m_pHdrMeasureConstants.p);
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 1, pUAVs, nullptr);
	m_pDeviceContext->Dispatch((r.Width() + 31) / 32, (r.Height() + 31) / 32, 1);

	// pass 2: the peak from the histogram, smoothed over time
	pSRV = nullptr;
	m_pDeviceContext->CSSetShaderResources(0, 1, &pSRV);
	m_pDeviceContext->CSSetShader(m_pCSHdrResolve, nullptr, 0);
	m_pDeviceContext->CSSetConstantBuffers(0, 1, &m_pHdrResolveConstants.p);
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 2, pUAVs, nullptr);
	m_pDeviceContext->Dispatch(1, 1, 1);

	// leave nothing bound: the state buffer is read as a shader resource next
	ID3D11UnorderedAccessView* pNullUAVs[2] = {};
	m_pDeviceContext->CSSetUnorderedAccessViews(0, 2, pNullUAVs, nullptr);
	m_pDeviceContext->CSSetShader(nullptr, nullptr, 0);

	return S_OK;
}

// Picks up the result of an earlier frame for the statistics. Never waits.
void CDX11VideoProcessor::ReadHdrMeasureStats()
{
	if (!m_bShowStats || !m_pHdrStateStaging || !m_pHdrStateBuffer) {
		m_bHdrStatsCopyPending = false;
		return;
	}

	if (m_bHdrStatsCopyPending) {
		D3D11_MAPPED_SUBRESOURCE mr;
		const HRESULT hr = m_pDeviceContext->Map(m_pHdrStateStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr);
		if (SUCCEEDED(hr)) {
			const float* p = static_cast<const float*>(mr.pData); // see the layout in cs_hdr_resolve.hlsl
			m_fHdrMeasuredPeakNits = p[4];
			m_fHdrSmoothedPeakNits = p[6];
			m_pDeviceContext->Unmap(m_pHdrStateStaging, 0);
			m_bHdrStatsCopyPending = false;
		} else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
			m_bHdrStatsCopyPending = false;
		}
	}

	if (!m_bHdrStatsCopyPending) {
		const D3D11_BOX box = { 0, 0, 0, 64, 1, 1 }; // the head of the state, not the window behind it
		m_pDeviceContext->CopySubresourceRegion(m_pHdrStateStaging, 0, 0, 0, 0, m_pHdrStateBuffer, 0, &box);
		m_bHdrStatsCopyPending = true;
	}
}

void CDX11VideoProcessor::SetShaderLuminanceParams()
{
	FLOAT cbuffer[4] = { 10000.0f / m_iSDRDisplayNits, GetSdrToneMappingPeak(), 0, 0 };

	if (m_pCorrectionConstants) {
		m_pDeviceContext->UpdateSubresource(m_pCorrectionConstants, 0, nullptr, &cbuffer, 0, 0);
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pCorrectionConstants));
	}
}

// Dolby Vision with L1 metadata takes ST 2094-10, unless it is set to the HDR method.
int CDX11VideoProcessor::HdrLocalToneMappingType() const
{
	if (m_DoviExtensionMetadata.L1.present && m_iHdrDoviToneMappingType == 6) {
		return 6;
	}
	return m_iHdrLocalToneMappingType;
}

// With the measured peak on, every method gets the measuring variant, which needs shader model 5.0.
// Without it the plain shader tone maps from the metadata.
HRESULT CDX11VideoProcessor::CreateHDR10ToneMappingShader()
{
	m_bHdrLocalMeasureActive = m_bHdrLocalMeasurePeak && HdrMeasureSupported() && SUCCEEDED(InitHdrMeasure());
	return CreatePShaderFromResource(&m_pPSHDR10ToneMapping,
		m_bHdrLocalMeasureActive ? IDF_PS_11_HDR10_TONEMAP_MEASURED : IDF_PS_11_HDR10_TONEMAP);
}

void CDX11VideoProcessor::SetHDR10ShaderParams(float masteringMinLuminanceNits, float masteringMaxLuminanceNits,
											   float maxCLL, float maxFALL,
											   float displayMaxNits, int toneMappingType)
{
	if (masteringMinLuminanceNits <= 0.f) masteringMinLuminanceNits = 0.f;
	if (masteringMaxLuminanceNits <= 10.f) masteringMaxLuminanceNits = 1000.f;
	if (maxCLL <= 10.f) maxCLL = masteringMaxLuminanceNits;
	if (maxFALL <= 1.f) maxFALL = maxCLL;
	if (displayMaxNits < 100.f || displayMaxNits > 10000.f) displayMaxNits = 1000.f;
	if (toneMappingType < 1 || toneMappingType > 7) toneMappingType = 1;

	const HDRParamsConstantBuffer_t cbuffer = {
		masteringMinLuminanceNits, masteringMaxLuminanceNits,
		maxCLL, maxFALL,
		displayMaxNits, static_cast<UINT>(toneMappingType)
	};

	if (!m_pPSHDR10ToneMapping) {
		EXECUTE_ASSERT(S_OK == CreateHDR10ToneMappingShader());
		DLogIf(m_pPSHDR10ToneMapping, L"CDX11VideoProcessor::SetHDR10ShaderParams() : m_pPSHDR10ToneMapping({}) created", m_iHdrLocalToneMappingType);

		UpdatePostScaleTexures();
		UpdateStatsStatic(); // whether the peak is measured is only known now
	}

	if (m_pHDR10ToneMappingConstants) {
		if (memcmp(&m_lastHDRParamsConstantBuffer, &cbuffer, sizeof(cbuffer)) == 0) {
			return;
		}

		m_pDeviceContext->UpdateSubresource(m_pHDR10ToneMappingConstants, 0, nullptr, &cbuffer, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pHDR10ToneMappingConstants);
		if (FAILED(result)) {
			DLog(L"SetHDR10ShaderLuminanceParams() failed to create m_pHDR10ToneMappingConstants. Error: {}", result);
		}
		EXECUTE_ASSERT(S_OK == result);
	}

	m_lastHDRParamsConstantBuffer = cbuffer;
}

void CDX11VideoProcessor::SetDolbyVisionDynamicParams()
{
	const DoViDynamicConstantsBuffer_t cbuffer = {
		m_DoviExtensionMetadata.L2.trim_chroma_weight - 0.5f, m_DoviExtensionMetadata.L2.trim_saturation_gain - 0.5f,
		m_DoviExtensionMetadata.L2.trim_slope + 0.5f, m_DoviExtensionMetadata.L2.trim_offset - 0.5f, m_DoviExtensionMetadata.L2.trim_power + 0.5f,
		static_cast<UINT>(m_DoviExtensionMetadata.L2.present)
	};

	if (m_pDoViDynamicConstants) {
		if (memcmp(&m_lastDoViDynamicConstantsBuffer, &cbuffer, sizeof(cbuffer)) == 0) {
			return;
		}

		m_pDeviceContext->UpdateSubresource(m_pDoViDynamicConstants, 0, nullptr, &cbuffer, 0, 0);
	} else {
		D3D11_BUFFER_DESC BufferDesc = {
			.ByteWidth = sizeof(cbuffer),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
		};
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		HRESULT result = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoViDynamicConstants);
		if (FAILED(result))
		{
			DLog(L"SetDolbyVisionDynamicParams() failed to create m_pDoViDynamicConstants. Error: {}", result);
		}
		EXECUTE_ASSERT(S_OK == result);
	}

	m_lastDoViDynamicConstantsBuffer = cbuffer;

#ifndef NDEBUG
	UpdateStatsStatic();
#endif
}

HRESULT CDX11VideoProcessor::SetShaderDoviCurvesPoly()
{
	ASSERT(m_Dovi.bValid);

	PS_DOVI_POLY_CURVE polyCurves[3] = {};

	const float scale = 1.0f / ((1 << m_Dovi.msd.Header.bl_bit_depth) - 1);
	const float scale_coef = 1.0f / (1u << m_Dovi.msd.Header.coef_log2_denom);

	for (int c = 0; c < 3; c++) {
		const auto& curve = m_Dovi.msd.Mapping.curves[c];
		auto& out = polyCurves[c];

		const int num_coef = curve.num_pivots - 1;
		bool has_poly = false;
		bool has_mmr = false;

		for (int i = 0; i < num_coef; i++) {
			switch (curve.mapping_idc[i]) {
			case 0: // polynomial
				has_poly = true;
				out.coeffs_data[i].x = scale_coef * curve.poly_coef[i][0];
				out.coeffs_data[i].y = (curve.poly_order[i] >= 1) ? scale_coef * curve.poly_coef[i][1] : 0.0f;
				out.coeffs_data[i].z = (curve.poly_order[i] >= 2) ? scale_coef * curve.poly_coef[i][2] : 0.0f;
				out.coeffs_data[i].w = 0.0f; // order=0 signals polynomial
				break;
			case 1: // mmr
				has_mmr = true;
				// not supported, leave as is
				out.coeffs_data[i].x = 0.0f;
				out.coeffs_data[i].y = 1.0f;
				out.coeffs_data[i].z = 0.0f;
				out.coeffs_data[i].w = 0.0f;
				break;
			}
		}

		const int n = curve.num_pivots - 2;
		for (int i = 0; i < n; i++) {
			out.pivots_data[i].x = scale * curve.pivots[i + 1];
		}
		for (int i = n; i < 7; i++) {
			out.pivots_data[i].x = 1e9f;
		}
	}

	HRESULT hr;

	if (m_pDoviCurvesConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pDoviCurvesConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (SUCCEEDED(hr)) {
			memcpy(mr.pData, &polyCurves, sizeof(polyCurves));
			m_pDeviceContext->Unmap(m_pDoviCurvesConstantBuffer, 0);
		}
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = { sizeof(polyCurves), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_SUBRESOURCE_DATA InitData = { &polyCurves, 0, 0 };
		hr = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoviCurvesConstantBuffer);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::SetShaderDoviCurves()
{
	ASSERT(m_Dovi.bValid);

	PS_DOVI_CURVE cbuffer[3] = {};

	for (int c = 0; c < 3; c++) {
		const auto& curve = m_Dovi.msd.Mapping.curves[c];
		auto& out = cbuffer[c];

		bool has_poly = false, has_mmr = false, mmr_single = true;
		uint32_t mmr_idx = 0, min_order = 3, max_order = 1;

		const float scale_coef = 1.0f / (1 << m_Dovi.msd.Header.coef_log2_denom);
		const int num_coef = curve.num_pivots - 1;
		for (int i = 0; i < num_coef; i++) {
			switch (curve.mapping_idc[i]) {
			case 0: // polynomial
				has_poly = true;
				out.coeffs_data[i].x = scale_coef * curve.poly_coef[i][0];
				out.coeffs_data[i].y = (curve.poly_order[i] >= 1) ? scale_coef * curve.poly_coef[i][1] : 0.0f;
				out.coeffs_data[i].z = (curve.poly_order[i] >= 2) ? scale_coef * curve.poly_coef[i][2] : 0.0f;
				out.coeffs_data[i].w = 0.0f; // order=0 signals polynomial
				break;
			case 1: // mmr
				min_order = std::min<int>(min_order, curve.mmr_order[i]);
				max_order = std::max<int>(max_order, curve.mmr_order[i]);
				mmr_single = !has_mmr;
				has_mmr = true;
				out.coeffs_data[i].x = scale_coef * curve.mmr_constant[i];
				out.coeffs_data[i].y = static_cast<float>(mmr_idx);
				out.coeffs_data[i].w = static_cast<float>(curve.mmr_order[i]);
				for (int j = 0; j < curve.mmr_order[i]; j++) {
					// store weights per order as two packed float4s
					out.mmr_data[mmr_idx].x = scale_coef * curve.mmr_coef[i][j][0];
					out.mmr_data[mmr_idx].y = scale_coef * curve.mmr_coef[i][j][1];
					out.mmr_data[mmr_idx].z = scale_coef * curve.mmr_coef[i][j][2];
					out.mmr_data[mmr_idx].w = 0.0f; // unused
					mmr_idx++;
					out.mmr_data[mmr_idx].x = scale_coef * curve.mmr_coef[i][j][3];
					out.mmr_data[mmr_idx].y = scale_coef * curve.mmr_coef[i][j][4];
					out.mmr_data[mmr_idx].z = scale_coef * curve.mmr_coef[i][j][5];
					out.mmr_data[mmr_idx].w = scale_coef * curve.mmr_coef[i][j][6];
					mmr_idx++;
				}
				break;
			}
		}

		const float scale = 1.0f / ((1 << m_Dovi.msd.Header.bl_bit_depth) - 1);
		const int n = curve.num_pivots - 2;
		for (int i = 0; i < n; i++) {
			out.pivots_data[i].x = scale * curve.pivots[i + 1];
		}
		for (int i = n; i < 7; i++) {
			out.pivots_data[i].x = 1e9f;
		}

		if (has_poly) {
			out.params.methods = PS_RESHAPE_POLY;
		}
		if (has_mmr) {
			out.params.methods |= PS_RESHAPE_MMR;
			out.params.mmr_single = mmr_single;
			out.params.min_order = min_order;
			out.params.max_order = max_order;
		}
	}

	HRESULT hr;

	if (m_pDoviCurvesConstantBuffer) {
		D3D11_MAPPED_SUBRESOURCE mr;
		hr = m_pDeviceContext->Map(m_pDoviCurvesConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
		if (SUCCEEDED(hr)) {
			memcpy(mr.pData, &cbuffer, sizeof(cbuffer));
			m_pDeviceContext->Unmap(m_pDoviCurvesConstantBuffer, 0);
		}
	}
	else {
		D3D11_BUFFER_DESC BufferDesc = { sizeof(cbuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_SUBRESOURCE_DATA InitData = { &cbuffer, 0, 0 };
		hr = m_pDevice->CreateBuffer(&BufferDesc, &InitData, &m_pDoviCurvesConstantBuffer);
	}

	return hr;
}

void CDX11VideoProcessor::UpdateTexParams(int cdepth)
{
	switch (m_iTexFormat) {
	case TEXFMT_AUTOINT:
		m_InternalTexFmt = (cdepth > 8 || m_bVPUseRTXVideoHDR) ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		break;
	case TEXFMT_8INT:    m_InternalTexFmt = DXGI_FORMAT_B8G8R8A8_UNORM;     break;
	case TEXFMT_10INT:   m_InternalTexFmt = DXGI_FORMAT_R10G10B10A2_UNORM;  break;
	case TEXFMT_16FLOAT: m_InternalTexFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
	default:
		ASSERT(FALSE);
	}
}

void CDX11VideoProcessor::UpdateRenderRect()
{
	m_renderRect.IntersectRect(m_videoRect, m_windowRect);
	UpdateScalingStrings();
}

void CDX11VideoProcessor::UpdateScalingStrings()
{
	const int w2 = m_videoRect.Width();
	const int h2 = m_videoRect.Height();
	const int k = m_bInterpolateAt50pct ? 2 : 1;
	int w1, h1;
	if (m_iRotation == 90 || m_iRotation == 270) {
		w1 = m_srcRectHeight;
		h1 = m_srcRectWidth;
	} else {
		w1 = m_srcRectWidth;
		h1 = m_srcRectHeight;
	}
	m_strShaderX = (w1 == w2) ? nullptr
		: (w1 > k * w2)
		? s_Downscaling11ResIDs[m_iDownscaling].description
		: s_Upscaling11ResIDs[m_iUpscaling].description;
	m_strShaderY = (h1 == h2) ? nullptr
		: (h1 > k * h2)
		? s_Downscaling11ResIDs[m_iDownscaling].description
		: s_Upscaling11ResIDs[m_iUpscaling].description;
}

void CDX11VideoProcessor::CalcStatsParams()
{
	if (m_pDeviceContext && !m_windowRect.IsRectEmpty()) {
		SIZE rtSize = m_windowRect.Size();
		m_StatsRect.top = m_StatsTextPoint.y - 5 + m_statsFeatureOffset;

		if (SUCCEEDED(m_Font3D.CreateFontBitmap(L"Consolas", m_StatsFontH, 0))) {
			SIZE charSize = m_Font3D.GetMaxCharMetric();
			m_StatsRect.right  = m_StatsRect.left + m_StatsColumns * charSize.cx + 5 + 3;
			m_StatsRect.bottom = m_StatsRect.top + m_StatsLines * charSize.cy + 5 + 3 + kStatsMarkerH + 1;
		}
		m_StatsBackground.Set(m_StatsRect, rtSize, D3DCOLOR_ARGB(80, 0, 0, 0));

		CalcGraphParams();
		m_Underlay.Set(m_GraphRect, rtSize, D3DCOLOR_ARGB(80, 0, 0, 0));

		m_Lines.ClearPoints(rtSize);
		POINT points[2];
		const int linestep = 20 * m_Yscale;
		for (int y = m_GraphRect.top + (m_Yaxis - m_GraphRect.top) % (linestep); y < m_GraphRect.bottom; y += linestep) {
			points[0] = { m_GraphRect.left,  y };
			points[1] = { m_GraphRect.right, y };
			m_Lines.AddPoints(points, std::size(points), (y == m_Yaxis) ? D3DCOLOR_XRGB(150, 150, 255) : D3DCOLOR_XRGB(100, 100, 255));
		}
		m_Lines.UpdateVertexBuffer();
	}
}

HRESULT CDX11VideoProcessor::MemCopyToTexSrcVideo(const BYTE* srcData, const int srcPitch)
{
	HRESULT hr = S_FALSE;
	D3D11_MAPPED_SUBRESOURCE mappedResource = {};

	if (m_TexSrcVideo.pTexture2) {
		hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			m_pCopyPlaneFn(m_srcHeight, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, srcPitch);
			m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture, 0);

			hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture2, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			if (SUCCEEDED(hr)) {
				const UINT cromaH = m_srcHeight / m_srcParams.pDX11Planes->div_chroma_h;
				const int cromaPitch = (m_TexSrcVideo.pTexture3) ? srcPitch / m_srcParams.pDX11Planes->div_chroma_w : srcPitch;
				srcData += srcPitch * m_srcHeight;
				m_pCopyPlaneFn(cromaH, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, cromaPitch);
				m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture2, 0);

				if (m_TexSrcVideo.pTexture3) {
					hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture3, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
					if (SUCCEEDED(hr)) {
						srcData += cromaPitch * cromaH;
						m_pCopyPlaneFn(cromaH, (BYTE*)mappedResource.pData, mappedResource.RowPitch, srcData, cromaPitch);
						m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture3, 0);
					}
				}
			}
		}
	} else {
		hr = m_pDeviceContext->Map(m_TexSrcVideo.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (SUCCEEDED(hr)) {
			const BYTE* src = (srcPitch < 0) ? srcData + srcPitch * (1 - (int)m_srcLines) : srcData;
			m_pCopyPlaneFn(m_srcLines, (BYTE*)mappedResource.pData, mappedResource.RowPitch, src, srcPitch);
			m_pDeviceContext->Unmap(m_TexSrcVideo.pTexture, 0);
		}
	}

	return hr;
}

HRESULT CDX11VideoProcessor::SetDevice(ID3D11Device *pDevice, ID3D11DeviceContext *pContext)
{
	DLog(L"CDX11VideoProcessor::SetDevice()");

	ReleaseSwapChain();
	m_pDXGIFactory2.Release();
	ReleaseDevice();

	CheckPointer(pDevice, E_POINTER);

	HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice));
	if (FAILED(hr)) {
		return hr;
	}
	if (pContext) {
		hr = pContext->QueryInterface(IID_PPV_ARGS(&m_pDeviceContext));
		if (FAILED(hr)) {
			return hr;
		}
	} else {
		m_pDevice->GetImmediateContext1(&m_pDeviceContext);
	}

	// for d3d11 subtitles
	CComQIPtr<ID3D10Multithread> pMultithread(m_pDeviceContext);
	pMultithread->SetMultithreadProtected(TRUE);

	CComPtr<IDXGIDevice> pDXGIDevice;
	hr = m_pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : QueryInterface(IDXGIDevice) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	CComPtr<IDXGIAdapter> pDXGIAdapter;
	hr = pDXGIDevice->GetAdapter(&pDXGIAdapter);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : GetAdapter(IDXGIAdapter) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	DXGI_ADAPTER_DESC dxgiAdapterDesc = {};
	hr = pDXGIAdapter->GetDesc(&dxgiAdapterDesc);
	if (SUCCEEDED(hr)) {
		m_VendorId = dxgiAdapterDesc.VendorId;
		m_strAdapterDescription = std::format(L"{} ({:04X}:{:04X})", dxgiAdapterDesc.Description, dxgiAdapterDesc.VendorId, dxgiAdapterDesc.DeviceId);
		DLog(L"Graphics DXGI adapter: {}", m_strAdapterDescription);
	}

	HRESULT hr2 = m_D3D11VP.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId);
	DLogIf(FAILED(hr2), L"CDX11VideoProcessor::SetDevice() : InitVideoDevice failed with error {}", HR2Str(hr2));

	D3D11_SAMPLER_DESC SampDesc = {};
	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	SampDesc.MinLOD = 0;
	SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerPoint));

	SampDesc.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT; // linear interpolation for magnification
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerLinear));

	SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateSamplerState(&SampDesc, &m_pSamplerDither));

	D3D11_BLEND_DESC bdesc = {};
	bdesc.RenderTarget[0].BlendEnable = TRUE;
	bdesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	bdesc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
	bdesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bdesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bdesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bdesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bdesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBlendState(&bdesc, &m_pAlphaBlendState));

	LPVOID data;
	DWORD size;
	EXECUTE_ASSERT(S_OK == GetDataFromResource(data, size, IDF_VS_11_SIMPLE));
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateVertexShader(data, size, nullptr, &m_pVS_Simple));

	D3D11_INPUT_ELEMENT_DESC Layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
	};
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateInputLayout(Layout, std::size(Layout), data, size, &m_pVSimpleInputLayout));

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_Simple, IDF_PS_11_SIMPLE));

	D3D11_BUFFER_DESC BufferDesc = { sizeof(VERTEX) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pVertexBuffer));

	BufferDesc = { sizeof(FLOAT) * 4 * 2, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pResizeShaderConstantBuffer));

	BufferDesc = { sizeof(FLOAT) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pHalfOUtoInterlaceConstantBuffer));
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pFinalPassConstantBuffer));

	BufferDesc = { sizeof(PS_EXTSHADER_CONSTANTS), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	EXECUTE_ASSERT(S_OK == m_pDevice->CreateBuffer(&BufferDesc, nullptr, &m_pPostScaleConstants));

	CComPtr<IDXGIFactory1> pDXGIFactory1;
	hr = pDXGIAdapter->GetParent(IID_PPV_ARGS(&pDXGIFactory1));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : GetParent(IDXGIFactory1) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	hr = pDXGIFactory1->QueryInterface(IID_PPV_ARGS(&m_pDXGIFactory2));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::SetDevice() : QueryInterface(IDXGIFactory2) failed with error {}", HR2Str(hr));
		ReleaseDevice();
		return hr;
	}

	HRESULT hr3 = m_Font3D.InitDeviceObjects(m_pDevice, m_pDeviceContext);
	DLogIf(FAILED(hr3), L"m_Font3D.InitDeviceObjects() failed with error {}", HR2Str(hr3));
	if (SUCCEEDED(hr3)) {
		hr3 = m_StatsBackground.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Rect3D.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_FeatureStatusBackground.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Underlay.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_Lines.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		hr3 = m_SyncLine.InitDeviceObjects(m_pDevice, m_pDeviceContext);
		DLogIf(FAILED(hr3), L"Geometric primitives InitDeviceObjects() failed with error {}", HR2Str(hr3));
	}
	ASSERT(S_OK == hr3);

	if (m_pFilter->m_inputMT.IsValid()) {
		if (!InitMediaType(&m_pFilter->m_inputMT)) {
			ReleaseDevice();
			return E_FAIL;
		}
	}

	if (m_hWnd) {
		hr = InitSwapChain(false);
		if (FAILED(hr)) {
			ReleaseDevice();
			return hr;
		}
	}

	SetCallbackDevice();
	UpdateSubPic();

	SetStereo3dTransform(m_iStereo3dTransform);

	HRESULT hr4 = m_TexDither.Create(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, dither_size, dither_size, Tex2D_DynamicShaderWrite);
	if (S_OK == hr4) {
		hr4 = GetDataFromResource(data, size, IDF_DITHER_32X32_FLOAT16);
		if (S_OK == hr4) {
			D3D11_MAPPED_SUBRESOURCE mappedResource;
			hr4 = m_pDeviceContext->Map(m_TexDither.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			if (S_OK == hr4) {
				uint16_t* src = (uint16_t*)data;
				BYTE* dst = (BYTE*)mappedResource.pData;
				for (UINT y = 0; y < dither_size; y++) {
					uint16_t* pUInt16 = reinterpret_cast<uint16_t*>(dst);
					for (UINT x = 0; x < dither_size; x++) {
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
						*pUInt16++ = src[x];
					}
					src += dither_size;
					dst += mappedResource.RowPitch;
				}
				m_pDeviceContext->Unmap(m_TexDither.pTexture, 0);
			}
		}
		if (FAILED(hr4)) {
			m_TexDither.Release();
		}
	}

	m_pFilter->OnDisplayModeChange();
	UpdateStatsStatic();
	UpdateStatsByWindow();
	UpdateStatsByDisplay();

	return hr;
}

HRESULT CDX11VideoProcessor::InitSwapChain(bool bWindowChanged)
{
	DLog(L"CDX11VideoProcessor::InitSwapChain() - {}", m_pFilter->m_bExclusiveScreen ? L"fullscreen" : L"window");
	CheckPointer(m_pDXGIFactory2, E_FAIL);

	ReleaseSwapChain();

	auto bFullscreenChange = m_bExclusiveScreen != m_pFilter->m_bExclusiveScreen;
	m_bExclusiveScreen = m_pFilter->m_bExclusiveScreen;

	if (bFullscreenChange || bWindowChanged) {
		HandleHDRToggle();
		UpdateBitmapShader();

		if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR10orHLG()) {
			m_bHdrAllowSwitchDisplay = false;
			InitMediaType(&m_pFilter->m_inputMT);
			m_bHdrAllowSwitchDisplay = true;

			if (m_pDXGISwapChain1) {
				DLog(L"CDX11VideoProcessor::InitSwapChain() - SwapChain was created during the call to InitMediaType(), exit");
				return S_OK;
			}
		}
	}

	const auto bHdrOutput = m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR);
	const auto b10BitOutput = bHdrOutput || Preferred10BitOutput();
	m_SwapChainFmt = b10BitOutput ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;

	HRESULT hr = S_OK;
	DXGI_SWAP_CHAIN_DESC1 desc1 = {};

	if (m_bExclusiveScreen) {
		MONITORINFOEXW mi = { sizeof(mi) };
		GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
		const CRect rc(mi.rcMonitor);

		desc1.Width = rc.Width();
		desc1.Height = rc.Height();
		desc1.Format = m_SwapChainFmt;
		desc1.SampleDesc.Count = 1;
		desc1.SampleDesc.Quality = 0;
		desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		if ((m_iSwapEffect == SWAPEFFECT_Flip && IsWindows8OrGreater()) || bHdrOutput) {
			desc1.BufferCount = bHdrOutput ? 6 : 2;
			desc1.Scaling = DXGI_SCALING_NONE;
			desc1.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		} else { // SWAPEFFECT_Discard or Windows 7
			desc1.BufferCount = 1;
			desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		}
		desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
		fullscreenDesc.RefreshRate.Numerator = 0;
		fullscreenDesc.RefreshRate.Denominator = 1;
		fullscreenDesc.Windowed = FALSE;

		g_bCreateSwapChain = true;
		hr = m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc1, &fullscreenDesc, nullptr, &m_pDXGISwapChain1);
		g_bCreateSwapChain = false;
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::InitSwapChain() : CreateSwapChainForHwnd(fullscreen) failed with error {}", HR2Str(hr));

		m_lastFullscreenHMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY);
	}
	else {
		desc1.Width = std::max(8, m_windowRect.Width());
		desc1.Height = std::max(8, m_windowRect.Height());
		desc1.Format = m_SwapChainFmt;
		desc1.SampleDesc.Count = 1;
		desc1.SampleDesc.Quality = 0;
		desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		if ((m_iSwapEffect == SWAPEFFECT_Flip && IsWindows8OrGreater()) || bHdrOutput) {
			desc1.BufferCount = bHdrOutput ? 6 : 2;
			desc1.Scaling = DXGI_SCALING_NONE;
			desc1.SwapEffect = IsWindows10OrGreater() ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		} else { // SWAPEFFECT_Discard or Windows 7
			desc1.BufferCount = 1;
			desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		}
		desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		DLogIf(m_windowRect.Width() < 8 || m_windowRect.Height() < 8,
			L"CDX11VideoProcessor::InitSwapChain() : Invalid window size {}x{}, use {}x{}",
			m_windowRect.Width(), m_windowRect.Height(), desc1.Width, desc1.Height);

		hr = m_pDXGIFactory2->CreateSwapChainForHwnd(m_pDevice, m_hWnd, &desc1, nullptr, nullptr, &m_pDXGISwapChain1);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::InitSwapChain() : CreateSwapChainForHwnd() failed with error {}", HR2Str(hr));

		m_lastFullscreenHMonitor = nullptr;
	}

	if (m_pDXGISwapChain1) {
		m_UsedSwapEffect = desc1.SwapEffect;

		HRESULT hr2 = m_pDXGISwapChain1->GetContainingOutput(&m_pDXGIOutput);

		m_currentSwapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
		if (bHdrOutput) {
			hr2 = m_pDXGISwapChain1->QueryInterface(IID_PPV_ARGS(&m_pDXGISwapChain4));

			if (m_pDXGIFactory1) {
				[&] {
					const auto hMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);

					CComPtr<IDXGIAdapter1> pDXGIAdapter;
					for (UINT i = 0; m_pDXGIFactory1->EnumAdapters1(i, &pDXGIAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
						CComPtr<IDXGIOutput> pDXGIOutput;
						for (UINT j = 0; pDXGIAdapter->EnumOutputs(j, &pDXGIOutput) != DXGI_ERROR_NOT_FOUND; ++j) {
							DXGI_OUTPUT_DESC desc;
							if (SUCCEEDED(pDXGIOutput->GetDesc(&desc)) && desc.Monitor == hMonitor) {
								CComPtr<IDXGIOutput6> pDXGIOutput6;
								if (SUCCEEDED(pDXGIOutput->QueryInterface(IID_PPV_ARGS(&pDXGIOutput6)))) {
									DXGI_OUTPUT_DESC1 desc;
									if (SUCCEEDED(pDXGIOutput6->GetDesc1(&desc))) {
										auto MaxLuminance = static_cast<UINT>(desc.MaxLuminance);
										if (MaxLuminance != m_MaxDisplayLuminance) {
											m_MaxDisplayLuminance = MaxLuminance;
											m_pFilter->UpdateDisplayInfo();
										}
									}
								}

								return;
							}

							pDXGIOutput.Release();
						}

						pDXGIAdapter.Release();
					}

				}();
			}
		}
	}

	return hr;
}

BOOL CDX11VideoProcessor::VerifyMediaType(const CMediaType* pmt)
{
	const auto& FmtParams = GetFmtConvParams(pmt);
	if (FmtParams.VP11Format == DXGI_FORMAT_UNKNOWN && FmtParams.DX11Format == DXGI_FORMAT_UNKNOWN) {
		return FALSE;
	}

	const BITMAPINFOHEADER* pBIH = GetBIHfromVIHs(pmt);
	if (!pBIH) {
		return FALSE;
	}

	if (pBIH->biWidth <= 0 || !pBIH->biHeight) {
		return FALSE;
	}

	return TRUE;
}

bool CDX11VideoProcessor::HandleHDRToggle()
{
	CAutoLock cRendererLock(&m_HDRToggleLock);

	m_bHdrDisplaySwitching = true;
	bool bRet = false;
	bool bEnableHDR = false;

	if (!m_bHDRModeChangeOutside) {
		if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR()) {
			MONITORINFOEXW mi = { sizeof(mi) };
			GetMonitorInfoW(m_lastFullscreenHMonitor ? m_lastFullscreenHMonitor : MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
			DisplayConfig_t displayConfig = {};

			if (GetDisplayConfig(mi.szDevice, displayConfig)) {
				if (displayConfig.HDRSupported() && m_iHdrToggleDisplay) {
					bool bHDREnabled = false;
					std::wstring deviceName(mi.szDevice);
					if (auto it = m_hdrModeStartState.find(deviceName); it != m_hdrModeStartState.end()) {
						bHDREnabled = it->second;
					}

					const bool bNeedToggleOn = !displayConfig.HDREnabled() &&
											   (m_iHdrToggleDisplay == HDRTD_On || m_iHdrToggleDisplay == HDRTD_OnOff
											   || m_bExclusiveScreen || m_bFullScreen && (m_iHdrToggleDisplay == HDRTD_On_Fullscreen || m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen));
					const bool bNeedToggleOff = displayConfig.HDREnabled() &&
												!bHDREnabled && (!m_bExclusiveScreen || !m_bFullScreen) && m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen;
					DLog(L"HandleHDRToggle() : {}, {}", bNeedToggleOn, bNeedToggleOff);
					if (bNeedToggleOn) {
						bRet = ToggleHDR(displayConfig, true);
						DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR ON failed");

						if (bRet) {
							bEnableHDR = true;
							m_hdrModeSavedState.try_emplace(std::move(deviceName), false);
						}
					} else if (bNeedToggleOff) {
						bRet = ToggleHDR(displayConfig, false);
						DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR OFF failed");

						if (bRet) {
							m_hdrModeSavedState.try_emplace(std::move(deviceName), true);
						}
					}
				}
			}
		} else if (m_iHdrToggleDisplay) {
			MONITORINFOEXW mi = { sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
			DisplayConfig_t displayConfig = {};

			if (GetDisplayConfig(mi.szDevice, displayConfig)) {
				// check if HDR was already enabled in Windows before starting
				bool bWindowsHDREnabled = false;
				std::wstring deviceName(mi.szDevice);
				if (auto it = m_hdrModeStartState.find(deviceName); it != m_hdrModeStartState.end()) {
					bWindowsHDREnabled = it->second;
				}

				if (displayConfig.HDRSupported() && displayConfig.HDREnabled() &&
						(!bWindowsHDREnabled || m_iHdrToggleDisplay == HDRTD_OnOff || (m_iHdrToggleDisplay == HDRTD_OnOff_Fullscreen && m_bExclusiveScreen))) {
					bRet = ToggleHDR(displayConfig, false);
					DLogIf(!bRet, L"CDX11VideoProcessor::HandleHDRToggle() : Toggle HDR OFF failed");

					if (bRet) {
						m_hdrModeSavedState.try_emplace(std::move(deviceName), true);
					}
				}
			}
		}
	}

	if (bRet) {
		Sleep(100);
		FillDisplayParams();

		for (int i = 0; i < 5 && bEnableHDR && !m_bHdrDisplayModeEnabled; i++) {
			Sleep(100);
			FillDisplayParams();
		}
	}

	m_bHdrDisplaySwitching = false;

	return bRet;
}

bool CDX11VideoProcessor::ToggleHDR(const DisplayConfig_t& displayConfig, const bool bEnableAdvancedColor)
{
	DLog(L"ToggleHDR() : {} for '{}'", bEnableAdvancedColor, displayConfig.displayName);

	auto GetCurrentDisplayMode = [](LPCWSTR lpszDeviceName) -> std::optional<DEVMODEW> {
		DEVMODEW devmode = {};
		devmode.dmSize = sizeof(DEVMODEW);
		auto ret = EnumDisplaySettingsW(lpszDeviceName, ENUM_CURRENT_SETTINGS, &devmode);
		if (ret) {
			return devmode;
		}

		return {};
	};

	auto beforeModeOpt = GetCurrentDisplayMode(displayConfig.displayName);

	LONG ret = 1;

	if (IsWindows11_24H2OrGreater()) {
		DISPLAYCONFIG_SET_HDR_STATE setHdrState = {};
		setHdrState.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE);
		setHdrState.header.size = sizeof(setHdrState);
		setHdrState.header.adapterId = displayConfig.modeTarget.adapterId;
		setHdrState.header.id = displayConfig.modeTarget.id;
		setHdrState.enableHdr = bEnableAdvancedColor ? 1 : 0;

		ret = DisplayConfigSetDeviceInfo(&setHdrState.header);
		DLogIf(ERROR_SUCCESS != ret, L"ToggleHDR() : DisplayConfigSetDeviceInfo(DISPLAYCONFIG_SET_HDR_STATE) with '{}' failed with error {}", bEnableAdvancedColor, HR2Str(HRESULT_FROM_WIN32(ret)));
	} else {
		DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setColorState = {};
		setColorState.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
		setColorState.header.size = sizeof(setColorState);
		setColorState.header.adapterId = displayConfig.modeTarget.adapterId;
		setColorState.header.id = displayConfig.modeTarget.id;
		setColorState.enableAdvancedColor = bEnableAdvancedColor ? 1 : 0;

		ret = DisplayConfigSetDeviceInfo(&setColorState.header);
		DLogIf(ERROR_SUCCESS != ret, L"ToggleHDR() : DisplayConfigSetDeviceInfo(DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE) with '{}' failed with error {}", bEnableAdvancedColor, HR2Str(HRESULT_FROM_WIN32(ret)));
	}

	if (ret == ERROR_SUCCESS) {
		m_bDisplayModeChangeAfterHDRToggle = true;

		if (beforeModeOpt.has_value()) {
			auto afterModeOpt = GetCurrentDisplayMode(displayConfig.displayName);
			if (afterModeOpt.has_value()) {
				auto& beforeMode = *beforeModeOpt;
				auto& afterMode = *afterModeOpt;
				if (beforeMode.dmPelsWidth != afterMode.dmPelsWidth || beforeMode.dmPelsHeight != afterMode.dmPelsHeight
						|| beforeMode.dmBitsPerPel != afterMode.dmBitsPerPel || beforeMode.dmDisplayFrequency != afterMode.dmDisplayFrequency) {
					DLog(L"ToggleHDR() : Display mode changed from {}x{}@{} to {}x{}@{}, restoring",
						 beforeMode.dmPelsWidth, beforeMode.dmPelsHeight, beforeMode.dmDisplayFrequency,
						 afterMode.dmPelsWidth, afterMode.dmPelsHeight, afterMode.dmDisplayFrequency);

					auto ret = ChangeDisplaySettingsExW(displayConfig.displayName, &beforeMode, nullptr, CDS_FULLSCREEN, nullptr);
					DLogIf(DISP_CHANGE_SUCCESSFUL != ret, L"ToggleHDR() : ChangeDisplaySettingsExW() failed with error {}", HR2Str(HRESULT_FROM_WIN32(ret)));

					m_bDisplayModeChangeAfterHDRToggle = true;
				}
			}
		}
	}

	return ret == ERROR_SUCCESS;
}

BOOL CDX11VideoProcessor::InitMediaType(const CMediaType* pmt)
{
	DLog(L"CDX11VideoProcessor::InitMediaType()");

	if (!VerifyMediaType(pmt)) {
		return FALSE;
	}

    ResetFeatureStatus();
	ResetSvdLutHistory();
	ReleaseVP();

	auto FmtParams = GetFmtConvParams(pmt);

	const BITMAPINFOHEADER* pBIH = nullptr;
	m_decExFmt.value = 0;

	if (pmt->formattype == FORMAT_VideoInfo2) {
		const VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)pmt->pbFormat;
		pBIH = &vih2->bmiHeader;
		m_srcRect = vih2->rcSource;
		m_srcAspectRatioX = vih2->dwPictAspectRatioX;
		m_srcAspectRatioY = vih2->dwPictAspectRatioY;
		if (FmtParams.CSType == CS_YUV && (vih2->dwControlFlags & (AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT))) {
			m_decExFmt.value = vih2->dwControlFlags;
			m_decExFmt.SampleFormat = AMCONTROL_USED | AMCONTROL_COLORINFO_PRESENT; // ignore other flags
		}
		m_bInterlaced = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced);
		m_rtAvgTimePerFrame = vih2->AvgTimePerFrame;
	}
	else if (pmt->formattype == FORMAT_VideoInfo) {
		const VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
		pBIH = &vih->bmiHeader;
		m_srcRect = vih->rcSource;
		m_srcAspectRatioX = 0;
		m_srcAspectRatioY = 0;
		m_bInterlaced = 0;
		m_rtAvgTimePerFrame = vih->AvgTimePerFrame;
	}
	else {
		return FALSE;
	}

	m_pFilter->m_FrameStats.SetStartFrameDuration(m_rtAvgTimePerFrame);
	m_mfgDisplayRate = {};
	m_pFilter->m_bValidBuffer = false;

	UINT biWidth  = pBIH->biWidth;
	UINT biHeight = labs(pBIH->biHeight);

	m_srcLines = biHeight * FmtParams.PitchCoeff / 2;
	m_srcPitch = biWidth * FmtParams.Packsize;
	switch (FmtParams.cformat) {
	case CF_YUY2:
	case CF_UYVY:
		m_srcPitch = ALIGN(m_srcPitch, 4);
		break;
	case CF_v210:
		m_srcPitch = ALIGN((biWidth + 5) / 6 * 16, 128);
		break;
	case CF_r210:
		m_srcPitch = ALIGN(m_srcPitch, 256); // 64 pixels
		break;
	default:
		if (pBIH->biCompression == BI_RGB) {
			m_srcPitch = ALIGN(m_srcPitch, 4);
			if (pBIH->biHeight > 0) {
				m_srcPitch = -m_srcPitch;
			}
		}
	}

	UINT origW = biWidth;
	UINT origH = biHeight;
	if (pmt->FormatLength() == VR_EXRADATA_POS + sizeof(VR_Extradata)) {
		const VR_Extradata* vrextra = reinterpret_cast<VR_Extradata*>(pmt->pbFormat + VR_EXRADATA_POS);
		if (vrextra->QueryWidth == pBIH->biWidth && vrextra->QueryHeight == pBIH->biHeight && vrextra->Compression == pBIH->biCompression) {
			origW  = vrextra->FrameWidth;
			origH = abs(vrextra->FrameHeight);
		}
	}

	if (m_srcRect.IsRectNull()) {
		m_srcRect.SetRect(0, 0, origW, origH);
	}
	m_srcRectWidth  = m_srcRect.Width();
	m_srcRectHeight = m_srcRect.Height();

	m_srcExFmt = SpecifyExtendedFormat(m_decExFmt, FmtParams, m_srcRectWidth, m_srcRectHeight);

	bool disableD3D11VP = false;
	switch (FmtParams.cformat) {
	case CF_NV12: disableD3D11VP = !m_VPFormats.bNV12; break;
	case CF_P010:
	case CF_P016: disableD3D11VP = !m_VPFormats.bP01x;  break;
	case CF_YUY2: disableD3D11VP = !m_VPFormats.bYUY2;  break;
	default:      disableD3D11VP = !m_VPFormats.bOther; break;
	}
	if (m_srcExFmt.VideoTransferMatrix == VIDEOTRANSFERMATRIX_YCgCo || m_Dovi.bValid) {
		disableD3D11VP = true;
	}
	if (FmtParams.CSType == CS_RGB && m_VendorId == PCIV_NVIDIA) {
		// D3D11 VP does not work correctly if RGB32 with odd frame width (source or target) on Nvidia adapters
		disableD3D11VP = true;
	}
	if (disableD3D11VP) {
		FmtParams.VP11Format = DXGI_FORMAT_UNKNOWN;
	}

	const auto frm_gcd = std::gcd(m_srcRectWidth, m_srcRectHeight);
	const auto srcFrameARX = m_srcRectWidth / frm_gcd;
	const auto srcFrameARY = m_srcRectHeight / frm_gcd;

	if (!m_srcAspectRatioX || !m_srcAspectRatioY) {
		m_srcAspectRatioX = srcFrameARX;
		m_srcAspectRatioY = srcFrameARY;
		m_srcAnamorphic = false;
	}
	else {
		const auto ar_gcd = std::gcd(m_srcAspectRatioX, m_srcAspectRatioY);
		m_srcAspectRatioX /= ar_gcd;
		m_srcAspectRatioY /= ar_gcd;
		m_srcAnamorphic = (srcFrameARX != m_srcAspectRatioX || srcFrameARY != m_srcAspectRatioY);
	}

	UpdateUpscalingShaders();
	UpdateDownscalingShaders();

	m_pPSCorrection.Release();
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();
	m_PSConvColorData.bEnable = false;

	m_pPSHDR10ToneMapping.Release();
	m_pHDR10ToneMappingConstants.Release();
	m_pDoViDynamicConstants.Release();

	UpdateTexParams(FmtParams.CDepth);

	if (m_bHdrAllowSwitchDisplay && m_srcVideoTransferFunction != m_srcExFmt.VideoTransferFunction) {
		auto ret = HandleHDRToggle();
		if (!ret && ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_bHdrPassthroughSupport && SourceIsHDR10orHLG() && !m_pDXGISwapChain4)) {
			ret = true;
		}
		if (ret) {
			ReleaseSwapChain();
			Init(m_hWnd, false);
		}
	}

	if (Preferred10BitOutput() && m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
		ReleaseSwapChain();
		Init(m_hWnd, false);
	}

	m_srcVideoTransferFunction = m_srcExFmt.VideoTransferFunction;

	HRESULT hr = E_NOT_VALID_STATE;

	// D3D11 Video Processor
	if (FmtParams.VP11Format != DXGI_FORMAT_UNKNOWN) {
		hr = InitializeD3D11VP(FmtParams, origW, origH, pmt);
		if (SUCCEEDED(hr)) {
			UINT resId = 0;
			m_pCorrectionConstants.Release();
			m_bSdrMeasureActive = false;
			bool bTransFunc22 = m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_22
								|| m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_709
								|| m_srcExFmt.VideoTransferFunction == DXVA2_VideoTransFunc_240M;

			if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084 && !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) && m_bConvertToSdr) {
				if (m_bSdrToneMapping) {
					// a separate shader, so the one used by the Fixed method is untouched
					m_bSdrMeasureActive = m_bSdrMeasurePeak && HdrMeasureSupported() && SUCCEEDED(InitHdrMeasure());
					if (m_bSdrMeasureActive) {
						resId = m_D3D11VP.IsPqSupported() ? IDF_PS_11_CONVERT_PQ_TO_SDR_TM_MEASURED : IDF_PS_11_FIXCONVERT_PQ_TO_SDR_TM_MEASURED;
						m_strCorrection = L"PQ to SDR, tone mapped, measured";
					} else {
						resId = m_D3D11VP.IsPqSupported() ? IDF_PS_11_CONVERT_PQ_TO_SDR_TM : IDF_PS_11_FIXCONVERT_PQ_TO_SDR_TM;
						m_strCorrection = L"PQ to SDR, tone mapped";
					}
				} else {
					resId = m_D3D11VP.IsPqSupported() ? IDF_PS_11_CONVERT_PQ_TO_SDR : IDF_PS_11_FIXCONVERT_PQ_TO_SDR;
					m_strCorrection = L"PQ to SDR";
				}
			}
			else if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) {
				if (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
					resId = IDF_PS_11_CONVERT_HLG_TO_PQ;
					m_strCorrection = L"HLG to PQ";
				}
				else if (m_bConvertToSdr) {
					if (m_bSdrToneMapping) {
						// HLG white is 1000 nits, which is what the peak falls back to without MaxCLL.
						// Measuring each frame is not offered here, since the histogram reads PQ.
						resId = IDF_PS_11_FIXCONVERT_HLG_TO_SDR_TM;
						m_strCorrection = L"HLG to SDR, tone mapped";
					} else {
						resId = IDF_PS_11_FIXCONVERT_HLG_TO_SDR;
						m_strCorrection = L"HLG to SDR";
					}
				}
				else if (m_srcExFmt.VideoPrimaries == MFVideoPrimaries_BT2020) {
					// HLG compatible with SDR
					resId = IDF_PS_11_FIX_BT2020;
					m_strCorrection = L"Fix BT.2020";
				}
			}
			else if ((bTransFunc22 || !m_bHdrPassthroughSupport) && m_srcExFmt.VideoPrimaries == MFVideoPrimaries_BT2020) {
				resId = IDF_PS_11_FIX_BT2020;
				m_strCorrection = L"Fix BT.2020";
			}

			if (resId) {
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, resId));
				DLogIf(m_pPSCorrection, L"CDX11VideoProcessor::InitMediaType() m_pPSCorrection('{}') created", m_strCorrection);
				SetShaderLuminanceParams();
			}
		}
		else {
			ReleaseVP();
		}
	}

	// Tex Video Processor
	if (FAILED(hr) && FmtParams.DX11Format != DXGI_FORMAT_UNKNOWN) {
		m_bVPUseRTXVideoHDR = false;
		hr = InitializeTexVP(FmtParams, origW, origH);
		if (SUCCEEDED(hr)) {
			SetShaderConvertColorParams();
			SetShaderLuminanceParams();
		}
	}

	if (SUCCEEDED(hr)) {
		UpdateBitmapShader();
		UpdateTexures();
		UpdatePostScaleTexures();
		UpdateStatsStatic();

		m_pFilter->m_inputMT = *pmt;

		return TRUE;
	}

	return FALSE;
}

HRESULT CDX11VideoProcessor::InitializeD3D11VP(const FmtConvParams_t& params, const UINT width, const UINT height, const CMediaType* pmt)
{
	if (!m_D3D11VP.IsVideoDeviceOk()) {
		return E_ABORT;
	}

	const auto& dxgiFormat = params.VP11Format;

	DLog(L"CDX11VideoProcessor::InitializeD3D11VP() started with input surface: {}, {} x {}", DXGIFormatToString(dxgiFormat), width, height);

	m_TexSrcVideo.Release();

	const bool bHdrPassthrough = m_bHdrDisplayModeEnabled && (SourceIsHDR10orHLG() || (m_bVPUseRTXVideoHDR && params.CDepth == 8));
	// Preserve Plus's eight-bit VP output for SDR NVIDIA VSR.
	m_D3D11OutputFmt = m_VendorId==PCIV_NVIDIA && m_iVPSuperRes && params.CDepth==8 && !SourceIsHDR()
		? DXGI_FORMAT_B8G8R8A8_UNORM : m_InternalTexFmt;
	const int deinterlacing = m_bInterlaced ? m_iVPDeinterlacing : DEINT_Disable;
	HRESULT hr = m_D3D11VP.InitVideoProcessor(dxgiFormat, width, height, m_srcExFmt, deinterlacing, bHdrPassthrough, m_D3D11OutputFmt);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : InitVideoProcessor() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = m_D3D11VP.InitInputTextures(m_pDevice);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : InitInputTextures() failed with error {}", HR2Str(hr));
		return hr;
	}

	auto rtxHDR = m_bVPRTXVideoHDR && m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_iTexFormat != TEXFMT_8INT && !SourceIsHDR();
	m_bVPUseRTXVideoHDR = (m_D3D11VP.SetRTXVideoHDR(rtxHDR) == S_OK);

	int superRes = SUPERRES_Disable;
	if (m_bVPScaling && !(m_bACMEnabled && !m_bVPUseRTXVideoHDR && params.CDepth == 8 && m_D3D11OutputFmt != DXGI_FORMAT_B8G8R8A8_UNORM)) {
		superRes = m_iVPSuperRes;
	}

	m_bVPUseSuperRes = (m_D3D11VP.SetSuperRes(superRes) == S_OK);

	if (m_hWnd && ((m_bVPUseRTXVideoHDR && !m_pDXGISwapChain4)
			|| (!m_bVPUseRTXVideoHDR && m_pDXGISwapChain4 && !SourceIsHDR()))) {
		InitSwapChain(false);
		InitMediaType(pmt);
		return S_OK;
	}

	hr = m_TexSrcVideo.Create(m_pDevice, dxgiFormat, width, height, Tex2D_DynamicShaderWriteNoSRV);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeD3D11VP() : m_TexSrcVideo.Create() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_srcWidth       = width;
	m_srcHeight      = height;
	m_srcParams      = params;
	m_srcDXGIFormat  = dxgiFormat;
	m_pCopyPlaneFn   = GetCopyPlaneFunction(params, VP_D3D11);

	DLog(L"CDX11VideoProcessor::InitializeD3D11VP() completed successfully");

	return S_OK;
}

HRESULT CDX11VideoProcessor::InitializeTexVP(const FmtConvParams_t& params, const UINT width, const UINT height)
{
	const auto& srcDXGIFormat = params.DX11Format;

	DLog(L"CDX11VideoProcessor::InitializeTexVP() started with input surface: {}, {} x {}", DXGIFormatToString(srcDXGIFormat), width, height);

	HRESULT hr = m_TexSrcVideo.CreateEx(m_pDevice, srcDXGIFormat, params.pDX11Planes, width, height, Tex2D_DynamicShaderWrite);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::InitializeTexVP() : m_TexSrcVideo.CreateEx() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_srcWidth       = width;
	m_srcHeight      = height;
	m_srcParams      = params;
	m_srcDXGIFormat  = srcDXGIFormat;
	m_pCopyPlaneFn   = GetCopyPlaneFunction(params, VP_D3D11_SHADER);

	// set default ProcAmp ranges
	SetDefaultDXVA2ProcAmpRanges(m_DXVA2ProcAmpRanges);

	hr = UpdateConvertColorShader();

	SAFE_RELEASE(m_PSConvColorData.pVertexBuffer);
	EXECUTE_ASSERT(S_OK == CreateVertexBuffer(m_pDevice, &m_PSConvColorData.pVertexBuffer, m_srcWidth, m_srcHeight, m_srcRect, 0, false));

	DLog(L"CDX11VideoProcessor::InitializeTexVP() completed successfully");

	return S_OK;
}

void CDX11VideoProcessor::UpdatFrameProperties()
{
	m_srcPitch = m_srcWidth * m_srcParams.Packsize;
	m_srcLines = m_srcHeight * m_srcParams.PitchCoeff / 2;
}

BOOL CDX11VideoProcessor::GetAlignmentSize(const CMediaType& mt, SIZE& Size)
{
	if (VerifyMediaType(&mt)) {
		const auto& FmtParams = GetFmtConvParams(&mt);

		if (FmtParams.cformat == CF_RGB24) {
			Size.cx = ALIGN(Size.cx, 4);
		}
		else if (FmtParams.cformat == CF_RGB48 || FmtParams.cformat == CF_BGR48) {
			Size.cx = ALIGN(Size.cx, 2);
		}
		else if (FmtParams.cformat == CF_BGRA64 || FmtParams.cformat == CF_B64A) {
			// nothing
		}
		else {
			auto pBIH = GetBIHfromVIHs(&mt);
			if (!pBIH) {
				return FALSE;
			}

			auto biWidth = pBIH->biWidth;
			auto biHeight = labs(pBIH->biHeight);

			if (!m_Alignment.cx || m_Alignment.cformat != FmtParams.cformat
					|| m_Alignment.texture.desc.Width != biWidth || m_Alignment.texture.desc.Height != biHeight) {
				m_Alignment.texture.Release();
				m_Alignment.cformat = {};
				m_Alignment.cx = {};
			}

			if (!m_Alignment.texture.pTexture) {
				auto VP11Format = FmtParams.VP11Format;
				if (VP11Format != DXGI_FORMAT_UNKNOWN) {
					bool disableD3D11VP = false;
					switch (FmtParams.cformat) {
						case CF_NV12: disableD3D11VP = !m_VPFormats.bNV12;  break;
						case CF_P010:
						case CF_P016: disableD3D11VP = !m_VPFormats.bP01x;  break;
						case CF_YUY2: disableD3D11VP = !m_VPFormats.bYUY2;  break;
						default:      disableD3D11VP = !m_VPFormats.bOther; break;
					}
					if (disableD3D11VP) {
						VP11Format = DXGI_FORMAT_UNKNOWN;
					}
				}

				HRESULT hr = E_FAIL;
				if (VP11Format != DXGI_FORMAT_UNKNOWN) {
					hr = m_Alignment.texture.Create(m_pDevice, VP11Format, biWidth, biHeight, Tex2D_DynamicShaderWriteNoSRV);
				}
				if (FAILED(hr) && FmtParams.DX11Format != DXGI_FORMAT_UNKNOWN) {
					hr = m_Alignment.texture.CreateEx(m_pDevice, FmtParams.DX11Format, FmtParams.pDX11Planes, biWidth, biHeight, Tex2D_DynamicShaderWrite);
				}
				if (FAILED(hr)) {
					return FALSE;
				}

				UINT RowPitch = 0;
				D3D11_MAPPED_SUBRESOURCE mappedResource = {};
				if (SUCCEEDED(m_pDeviceContext->Map(m_Alignment.texture.pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
					RowPitch = mappedResource.RowPitch;
					m_pDeviceContext->Unmap(m_Alignment.texture.pTexture, 0);
				}

				if (!RowPitch) {
					return FALSE;
				}

				m_Alignment.cformat = FmtParams.cformat;
				m_Alignment.cx = RowPitch / FmtParams.Packsize;
			}

			Size.cx = m_Alignment.cx;
		}

		if (FmtParams.cformat == CF_RGB24 || FmtParams.cformat == CF_XRGB32 || FmtParams.cformat == CF_ARGB32) {
			Size.cy = -abs(Size.cy); // only for biCompression == BI_RGB
		} else {
			Size.cy = abs(Size.cy);
		}

		return TRUE;

	}

	return FALSE;
}

namespace {
class LookaheadSample final : public CMediaSample, public IMediaSampleD3D11 {
    CComPtr<CMemAllocator> m_allocator;
    CComPtr<ID3D11Texture2D> m_texture;
public:
    LookaheadSample(CMemAllocator* allocator, ID3D11Texture2D* texture)
        : CMediaSample(L"ZipDepth lookahead sample", allocator, nullptr), m_allocator(allocator), m_texture(texture) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IMediaSampleD3D11)) return GetInterface(static_cast<IMediaSampleD3D11*>(this), out);
        return CMediaSample::QueryInterface(iid, out);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return CMediaSample::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&m_cRef);
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP GetD3D11Texture(int view, ID3D11Texture2D** texture, UINT* slice) override {
        if (!texture || !slice) return E_POINTER;
        if (view) return E_INVALIDARG;
        *slice = 0;
        return m_texture.CopyTo(texture);
    }
};
}

CComPtr<IMediaSample> CDX11VideoProcessor::CloneLookaheadSample(IMediaSample* sample, int64_t& pts, uint64_t& bytes)
{
    if (CanProjectJvr() || !m_pDeviceContext || !m_pDevice || (!UsesZipDepthMfg() && !UsesZipDepthSr()) ||
        !MfgDepthSourceEligible() || m_bInterlaced || m_srcDXGIFormat != DXGI_FORMAT_NV12 ||
        m_srcExFmt.VideoTransferMatrix != DXVA2_VideoTransferMatrix_BT709 ||
        sample->IsDiscontinuity() == S_OK || sample->IsPreroll() == S_OK)
        return {};
    REFERENCE_TIME end = 0;
    if (sample->GetTime(&pts, &end) != S_OK || pts < 0 || end <= pts) return {};
    AM_MEDIA_TYPE* changedType = nullptr;
    if (sample->GetMediaType(&changedType) == S_OK) {
        DeleteMediaType(changedType);
        return {};
    }
    if (CComQIPtr<IMediaSideData> sideData = sample) {
        const BYTE* data = nullptr;
        size_t size = 0;
        if (SUCCEEDED(sideData->GetSideData(IID_MediaSideDataDOVIMetadataV2, &data, &size)) && size) return {};
    }
    const CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> input;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &input, &slice)) || !input) return {};
    CComPtr<ID3D11Device> owner;
    input->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return {};
    D3D11_TEXTURE2D_DESC desc{};
    input->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || desc.MipLevels != 1 || slice >= desc.ArraySize ||
        desc.Width < m_srcWidth || desc.Height < m_srcHeight)
        return {};
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (m_D3D11VP.IsReady()) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    CComPtr<ID3D11Texture2D> copy;
    if (FAILED(m_pDevice->CreateTexture2D(&desc, nullptr, &copy))) return {};
    // Check the VP view before acknowledging a sample as queued for later use.
    if (m_D3D11VP.IsReady() && FAILED(m_D3D11VP.CheckInputTexture(copy))) return {};
    if (!m_lookaheadAllocator) {
        HRESULT hr = S_OK;
        auto* allocator = new(std::nothrow) CMemAllocator(L"ZipDepth lookahead", nullptr, &hr);
        if (!allocator || FAILED(hr)) {
            delete allocator;
            return {};
        }
        m_lookaheadAllocator = allocator;
    }
    auto* packet = new(std::nothrow) LookaheadSample(m_lookaheadAllocator, copy);
    if (!packet) return {};
    CComPtr<IMediaSample> result(static_cast<IMediaSample*>(packet));
    packet->SetTime(&pts, &end);
    packet->SetSyncPoint(sample->IsSyncPoint() == S_OK);
    LONGLONG mediaStart = 0, mediaEnd = 0;
    if (sample->GetMediaTime(&mediaStart, &mediaEnd) == S_OK) packet->SetMediaTime(&mediaStart, &mediaEnd);
    m_pDeviceContext->CopySubresourceRegion(copy, 0, 0, 0, 0, input, slice, nullptr);
    bytes = uint64_t(desc.Width) * desc.Height * 3 / 2;
    return result;
}

bool CDX11VideoProcessor::PrefetchZipDepth(IMediaSample* sample, int64_t pts)
{
    if (CanProjectJvr() || (!UsesZipDepthMfg() && !UsesZipDepthSr()) || !m_pDevice || !m_pDeviceContext || !MfgDepthSourceEligible())
        return true;
    CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &texture, &slice)) || !texture || slice) return true;
    CComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return true;
    return m_zipDepth.PrefetchNv12(m_pDevice, m_pDeviceContext, texture, m_srcRect,
        m_srcExFmt.NominalRange == DXVA2_NominalRange_0_255, pts);
}

HRESULT CDX11VideoProcessor::ProcessSample(IMediaSample* pSample)
{
	// The decoder can explicitly mark a discontinuity even when timestamps
	// remain monotonic (e.g. a stream splice or a new segment). Never allow
	// DLSS SR to reproject history across that boundary. Restart duplicate
	// detection and interpolation history after a splice or new segment.
	if (pSample->IsDiscontinuity() == S_OK) {
		if (m_mfgMultiplier) {
			StopMfg(false);
			m_mfg.ResetHistory();
			m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
		}
		m_dlssSR.ResetHistory();
		m_zipDepth.ResetHistory();
		m_D3D11VP.ResetFrameOrder();
	}
	REFERENCE_TIME rtStart = 0, rtEnd = 0;
	const HRESULT timing = pSample->GetTime(&rtStart, &rtEnd);
	if (FAILED(timing)) {
		rtStart = m_pFilter->m_FrameStats.GeTimestamp();
	}
	const REFERENCE_TIME rtFrameDur = timing == S_OK && rtEnd > rtStart
		? rtEnd - rtStart : m_pFilter->m_FrameStats.GetAverageFrameDuration();
	rtEnd = rtStart + rtFrameDur;

	m_rtStart = rtStart;
	m_sampleDuration = rtFrameDur;
	m_zipDepth.PlaybackFrame(rtStart, rtFrameDur);
	CRefTime rtClock(rtStart);

	HRESULT hr = CopySample(pSample);
	if (FAILED(hr)) {
		m_RenderStats.failed++;
		return hr;
	}

	// always Render(1) a frame after CopySample()
	hr = Render(1, rtStart);
	if (m_completingSample) return hr;
	m_pFilter->m_DrawStats.Add(GetPreciseTick());
	if (m_pFilter->m_filterState == State_Running) {
		m_pFilter->StreamTime(rtClock);
	}

	m_RenderStats.syncoffset = rtClock - rtStart;

	int so = (int)std::clamp(m_RenderStats.syncoffset, -UNITS, UNITS);
#if SYNC_OFFSET_EX
	m_SyncDevs.Add(so - m_Syncs.Last());
#endif
	m_Syncs.Add(so);

	if (m_bDoubleFrames && SUCCEEDED(hr)) {
		if (rtEnd < rtClock) {
			m_RenderStats.dropped2++;
			return S_FALSE; // skip frame
		}

		rtStart += rtFrameDur / 2;

		hr = Render(2, rtStart);
		m_pFilter->m_DrawStats.Add(GetPreciseTick());
		if (m_pFilter->m_filterState == State_Running) {
			m_pFilter->StreamTime(rtClock);
		}

		m_RenderStats.syncoffset = rtClock - rtStart;

		so = (int)std::clamp(m_RenderStats.syncoffset, -UNITS, UNITS);
#if SYNC_OFFSET_EX
		m_SyncDevs.Add(so - m_Syncs.Last());
#endif
		m_Syncs.Add(so);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::CopySample(IMediaSample* pSample)
{
	CheckPointer(m_pDXGISwapChain1, E_FAIL);

	uint64_t tick = GetPreciseTick();

	// Get frame type
	m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; // Progressive
	m_bDoubleFrames = false;
	if (m_bInterlaced) {
		if (CComQIPtr<IMediaSample2> pMS2 = pSample) {
			AM_SAMPLE2_PROPERTIES props;
			if (SUCCEEDED(pMS2->GetProperties(sizeof(props), (BYTE*)&props))) {
				if ((props.dwTypeSpecificFlags & AM_VIDEO_FLAG_WEAVE) == 0) {
					if (props.dwTypeSpecificFlags & AM_VIDEO_FLAG_FIELD1FIRST) {
						m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST; // Top-field first
					} else {
						m_SampleFormat = D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST; // Bottom-field first
					}
					m_bDoubleFrames = m_iVPDeinterlacing && m_bDeintDouble && m_D3D11VP.IsReady();
				}
			}
		}
	}

	HRESULT hr = S_OK;
	m_FieldDrawn = 0;
	bool updateStats = false;

	m_hdr10 = {};
	if (CComQIPtr<IMediaSideData> pMediaSideData = pSample) {
		// m_bSdrToneMapping needs it too, and that path runs with the other two off
		if (SourceIsHDR10orHLG() && (m_bHdrPassthrough || m_bHdrLocalToneMapping || m_bSdrToneMapping)) {
			MediaSideDataHDR* hdr = nullptr;
			size_t size = 0;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataHDR, (const BYTE**)&hdr, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataHDR)) {
				const auto& primaries_x = hdr->display_primaries_x;
				const auto& primaries_y = hdr->display_primaries_y;
				if (primaries_x[0] > 0. && primaries_x[1] > 0. && primaries_x[2] > 0.
						&& primaries_y[0] > 0. && primaries_y[1] > 0. && primaries_y[2] > 0.
						&& hdr->white_point_x > 0. && hdr->white_point_y > 0.
						&& hdr->max_display_mastering_luminance > 0. && hdr->min_display_mastering_luminance > 0.) {
					m_hdr10.bValid = true;

					m_hdr10.hdr10.RedPrimary[0]   = static_cast<UINT16>(std::lround(primaries_x[2] * 50000.0));
					m_hdr10.hdr10.RedPrimary[1]   = static_cast<UINT16>(std::lround(primaries_y[2] * 50000.0));
					m_hdr10.hdr10.GreenPrimary[0] = static_cast<UINT16>(std::lround(primaries_x[0] * 50000.0));
					m_hdr10.hdr10.GreenPrimary[1] = static_cast<UINT16>(std::lround(primaries_y[0] * 50000.0));
					m_hdr10.hdr10.BluePrimary[0]  = static_cast<UINT16>(std::lround(primaries_x[1] * 50000.0));
					m_hdr10.hdr10.BluePrimary[1]  = static_cast<UINT16>(std::lround(primaries_y[1] * 50000.0));
					m_hdr10.hdr10.WhitePoint[0]   = static_cast<UINT16>(std::lround(hdr->white_point_x * 50000.0));
					m_hdr10.hdr10.WhitePoint[1]   = static_cast<UINT16>(std::lround(hdr->white_point_y * 50000.0));

					m_hdr10.hdr10.MaxMasteringLuminance = static_cast<UINT>(std::lround(hdr->max_display_mastering_luminance));
					m_hdr10.hdr10.MinMasteringLuminance = static_cast<UINT>(std::lround(hdr->min_display_mastering_luminance * 10000.0));
				}
			}

			MediaSideDataHDRContentLightLevel* hdrCLL = nullptr;
			size = 0;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataHDRContentLightLevel, (const BYTE**)&hdrCLL, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataHDRContentLightLevel)) {
				m_hdr10.hdr10.MaxContentLightLevel      = hdrCLL->MaxCLL;
				m_hdr10.hdr10.MaxFrameAverageLightLevel = hdrCLL->MaxFALL;
				if (m_bSdrToneMapping) {
					SetShaderLuminanceParams(); // the curve is built from this
				}
			}
		}

		size_t size = 0;
		MediaSideData3DOffset* offset = nullptr;
		hr = pMediaSideData->GetSideData(IID_MediaSideData3DOffset, (const BYTE**)&offset, &size);
		if (SUCCEEDED(hr) && size == sizeof(MediaSideData3DOffset) && offset->offset_count > 0 && offset->offset[0]) {
			m_nStereoSubtitlesOffsetInPixels = offset->offset[0];
		}

		if (m_srcParams.CSType == CS_YUV && (m_bHdrPreferDoVi || !SourceIsHDR10orHLG())) {
			MediaSideDataDOVIMetadata* pDOVIMetadata = nullptr;
			hr = pMediaSideData->GetSideData(IID_MediaSideDataDOVIMetadataV2, (const BYTE**)&pDOVIMetadata, &size);
			if (SUCCEEDED(hr) && size == sizeof(MediaSideDataDOVIMetadata) && CheckDoviMetadata(pDOVIMetadata, 1)) {
				const bool bYCCtoRGBChanged = !m_PSConvColorData.bEnable ||
					(memcmp(
						&m_Dovi.msd.ColorMetadata.ycc_to_rgb_matrix,
						&pDOVIMetadata->ColorMetadata.ycc_to_rgb_matrix,
						sizeof(MediaSideDataDOVIMetadata::ColorMetadata.ycc_to_rgb_matrix) + sizeof(MediaSideDataDOVIMetadata::ColorMetadata.ycc_to_rgb_offset)
					) != 0);
				const bool bRGBtoLMSChanged =
					(memcmp(
						&m_Dovi.msd.ColorMetadata.rgb_to_lms_matrix,
						&pDOVIMetadata->ColorMetadata.rgb_to_lms_matrix,
						sizeof(MediaSideDataDOVIMetadata::ColorMetadata.rgb_to_lms_matrix)
					) != 0);
				const bool bMappingCurvesChanged = !m_pDoviCurvesConstantBuffer ||
					(memcmp(
						&m_Dovi.msd.Mapping.curves,
						&pDOVIMetadata->Mapping.curves,
						sizeof(MediaSideDataDOVIMetadata::Mapping.curves)
					) != 0);

				const bool bMasteringLuminanceChanged = m_Dovi.msd.ColorMetadata.source_max_pq != pDOVIMetadata->ColorMetadata.source_max_pq
					|| m_Dovi.msd.ColorMetadata.source_min_pq != pDOVIMetadata->ColorMetadata.source_min_pq;

				bool bMMRChanged = false;
				if (bMappingCurvesChanged) {
					bool has_mmr = false;
					for (const auto& curve : pDOVIMetadata->Mapping.curves) {
						for (uint8_t i = 0; i < (curve.num_pivots - 1); i++) {
							if (curve.mapping_idc[i] == 1) {
								has_mmr = true;
								break;
							}
						}
					}
					if (m_Dovi.bHasMMR != has_mmr) {
						m_Dovi.bHasMMR = has_mmr;
						m_pDoviCurvesConstantBuffer.Release();
						bMMRChanged = true;
					}
				}

				memcpy(&m_Dovi.msd, pDOVIMetadata, size);
				const bool doviStateChanged = !m_Dovi.bValid;
				m_Dovi.bValid = true;

				// based on libplacebo source code
				constexpr float
					PQ_M1 = 2610.f / (4096.f * 4.f),
					PQ_M2 = 2523.f / 4096.f * 128.f,
					PQ_C1 = 3424.f / 4096.f,
					PQ_C2 = 2413.f / 4096.f * 32.f,
					PQ_C3 = 2392.f / 4096.f * 32.f;

				auto PqToLinearNits = [](float x) {
					x = powf(x, 1.0f / PQ_M2);
					x = fmaxf(x - PQ_C1, 0.0f) / (PQ_C2 - PQ_C3 * x);
					x = powf(x, 1.0f / PQ_M1);
					return x * 10000.0f;
				};
				auto LinearNitsToPq = [](float y) {
					y /= 10000.0f;
					y = fmaxf(y, 0.0f);
					y = powf(y, PQ_M1);
					y = (PQ_C1 + PQ_C2 * y) / (1.0f + PQ_C3 * y);
					return powf(y, PQ_M2);
				};

				// Level 1 + 3
				for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
					if (pDOVIMetadata->Extensions[i].level == 1) {
						auto& Level1 = pDOVIMetadata->Extensions[i].Level1;

						m_DoviExtensionMetadata.L1.present = true;
						m_DoviExtensionMetadata.L1.min_pq = Level1.min_pq;
						m_DoviExtensionMetadata.L1.max_pq = Level1.max_pq;
						m_DoviExtensionMetadata.L1.avg_pq = Level1.avg_pq;

						for (uint32_t k = 0; k < LAV_DOVI_MAX_EXTENSIONS; ++k) {
							if (pDOVIMetadata->Extensions[k].level == 3) {
								auto& Level3 = pDOVIMetadata->Extensions[k].Level3;

								m_DoviExtensionMetadata.L1.min_pq = m_DoviExtensionMetadata.L1.min_pq + Level3.min_pq_offset - 2048;
								m_DoviExtensionMetadata.L1.max_pq = m_DoviExtensionMetadata.L1.max_pq + Level3.max_pq_offset - 2048;
								m_DoviExtensionMetadata.L1.avg_pq = m_DoviExtensionMetadata.L1.avg_pq + Level3.avg_pq_offset - 2048;

								break;
							}
						}

						m_DoviExtensionMetadata.L1.min_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.min_pq / 4095.f));
						m_DoviExtensionMetadata.L1.max_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.max_pq / 4095.f));
						m_DoviExtensionMetadata.L1.avg_pq = static_cast<UINT>(PqToLinearNits(m_DoviExtensionMetadata.L1.avg_pq / 4095.f));

						if (m_bHdrPassthroughSupport && m_bHdrLocalToneMapping) {
							if (m_DoviExtensionMetadata.L1 != m_DoviExtensionMetadata.L1Cached) {
								m_DoviExtensionMetadata.L1Cached = m_DoviExtensionMetadata.L1;
								UpdateStatsStatic();
							}
						}

						break;
					}
				}

				// Level 2
				float display_pq = LinearNitsToPq(m_iHdrDisplayMaxNits);
				int lower_index = -1, upper_index = -1;
				float closest_lower_dist = 1.0f, closest_upper_dist = 1.0f;
				bool level2Present = false;
				uint16_t max_target_max_pq = std::numeric_limits<uint16_t>::max();

				for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
					if (pDOVIMetadata->Extensions[i].level == 2) {
						level2Present = true;

						auto& Level2 = pDOVIMetadata->Extensions[i].Level2;

						if (!m_bHdrPassthroughSupport) {
							if (Level2.target_max_pq < max_target_max_pq) {
								max_target_max_pq = Level2.target_max_pq;
								upper_index = i;
							}
						} else {
							float target_pq = Level2.target_max_pq / 4095.0f;
							if (target_pq <= display_pq) {
								float dist = display_pq - target_pq;
								if (dist < closest_lower_dist) {
									closest_lower_dist = dist;
									lower_index = i;
								}
							} else {
								float dist = target_pq - display_pq;
								if (dist < closest_upper_dist) {
									closest_upper_dist = dist;
									upper_index = i;
								}
							}
						}
					}
				}

				if (level2Present) {
					float t_slope = 1.0f, t_offset = 0.0f, t_power = 1.0f;
					float t_chroma = 0.0f, t_sat = 0.0f;

					// SCENARIO A: Display is BETWEEN two targets
					if (lower_index != -1 && upper_index != -1) {
						float lower_pq = pDOVIMetadata->Extensions[lower_index].Level2.target_max_pq / 4095.0f;
						float upper_pq = pDOVIMetadata->Extensions[upper_index].Level2.target_max_pq / 4095.0f;

						float weight = (upper_pq != lower_pq) ? (display_pq - lower_pq) / (upper_pq - lower_pq) : 0.0f;
						weight = std::clamp(weight, 0.0f, 1.0f);

						t_slope = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_slope),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_slope),
											weight);
						t_offset = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_offset),
												static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_offset),
												weight);
						t_power = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_power),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_power),
											weight);
						t_chroma = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_chroma_weight),
												static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_chroma_weight),
												weight);
						t_sat = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_saturation_gain),
											static_cast<float>(pDOVIMetadata->Extensions[upper_index].Level2.trim_saturation_gain),
											weight);
					}
					// SCENARIO B: Display is BRIGHTER than all targets (Interpolate towards Master/Neutral)
					else if (lower_index != -1 && upper_index == -1) {
						float master_pq = pDOVIMetadata->ColorMetadata.source_max_pq / 4095.0f;

						float lower_pq = pDOVIMetadata->Extensions[lower_index].Level2.target_max_pq / 4095.0f;
						float weight = (master_pq > lower_pq) ? (display_pq - lower_pq) / (master_pq - lower_pq) : 0.0f;
						weight = std::clamp(weight, 0.0f, 1.0f);

						t_slope = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_slope), 2048.0f, weight);
						t_offset = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_offset), 2048.0f, weight);
						t_power = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_power), 2048.0f, weight);
						t_chroma = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_chroma_weight), 2048.0f, weight);
						t_sat = std::lerp(static_cast<float>(pDOVIMetadata->Extensions[lower_index].Level2.trim_saturation_gain), 2048.0f, weight);
					}
					// SCENARIO C: Display is DIMMER than all targets (Clamp to the lowest available target)
					else if (lower_index == -1 && upper_index != -1) {
						t_slope = pDOVIMetadata->Extensions[upper_index].Level2.trim_slope;
						t_offset = pDOVIMetadata->Extensions[upper_index].Level2.trim_offset;
						t_power = pDOVIMetadata->Extensions[upper_index].Level2.trim_power;
						t_chroma = pDOVIMetadata->Extensions[upper_index].Level2.trim_chroma_weight;
						t_sat = pDOVIMetadata->Extensions[upper_index].Level2.trim_saturation_gain;
					}

					// Final normalization to floating point coefficients
					m_DoviExtensionMetadata.L2.present = true;
					m_DoviExtensionMetadata.L2.trim_slope = t_slope / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_offset = t_offset / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_power = t_power / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_saturation_gain = t_sat / 4096.0f;
					m_DoviExtensionMetadata.L2.trim_chroma_weight = t_chroma / 4096.0f;
				}

				SetDolbyVisionDynamicParams();

				if (bMasteringLuminanceChanged) {
					m_DoviMaxMasteringLuminance = static_cast<UINT>(PqToLinearNits(m_Dovi.msd.ColorMetadata.source_max_pq / 4095.f));
					m_DoviMinMasteringLuminance = static_cast<UINT>(PqToLinearNits(m_Dovi.msd.ColorMetadata.source_min_pq / 4095.f) * 10000.0f);

					for (uint32_t i = 0; i < LAV_DOVI_MAX_EXTENSIONS; ++i) {
						if (pDOVIMetadata->Extensions[i].level == 6) {
							auto& Level6 = pDOVIMetadata->Extensions[i].Level6;

							m_DoviMaxMasteringLuminance = Level6.max_luminance;
							m_DoviMinMasteringLuminance = Level6.min_luminance;
							m_DoviMaxContentLightLevel = Level6.max_cll;
							m_DoviMaxFrameAverageLightLevel = Level6.max_fall;

							break;
						}
					}
				}

				if (m_bSdrToneMapping && (doviStateChanged || bMasteringLuminanceChanged)) {
					SetShaderLuminanceParams(); // the curve is built from the levels in the RPU
				}

				if (m_D3D11VP.IsReady()) {
					InitMediaType(&m_pFilter->m_inputMT);
				}
				else if (doviStateChanged) {
					UpdateStatsStatic();
				}

				if (bYCCtoRGBChanged) {
					DLog(L"CDX11VideoProcessor::CopySample() : DoVi ycc_to_rgb_matrix is changed");
					SetShaderConvertColorParams();
				}
				if (bRGBtoLMSChanged || bMMRChanged) {
					DLogIf(bRGBtoLMSChanged, L"CDX11VideoProcessor::CopySample() : DoVi rgb_to_lms_matrix is changed");
					DLogIf(bMMRChanged, L"CDX11VideoProcessor::CopySample() : DoVi has_mmr is changed");
					UpdateConvertColorShader();
				}
				if (bMappingCurvesChanged) {
					if (m_Dovi.bHasMMR) {
						hr = SetShaderDoviCurves();
					} else {
						hr = SetShaderDoviCurvesPoly();
					}
				}

				if (doviStateChanged && !SourceIsHDR10orHLG()) {
					ReleaseSwapChain();
					Init(m_hWnd, false);

					m_srcVideoTransferFunction = 0;
					InitMediaType(&m_pFilter->m_inputMT);
				}
			}
		}
	}

	bool comparedCpuNv12 = false;
	if (CComQIPtr<IMediaSampleD3D11> pMSD3D11 = pSample) {
		if (m_iSrcFromGPU != 11) {
			m_iSrcFromGPU = 11;
			updateStats = true;
		}

		CComQIPtr<ID3D11Texture2D> pD3D11Texture2D;
		UINT ArraySlice = 0;
		hr = pMSD3D11->GetD3D11Texture(0, &pD3D11Texture2D, &ArraySlice);
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::CopySample() : GetD3D11Texture() failed with error {}", HR2Str(hr));
			return hr;
		}
		if (!pD3D11Texture2D) return E_POINTER;

		D3D11_TEXTURE2D_DESC desc = {};
		pD3D11Texture2D->GetDesc(&desc);
		if (desc.Format != m_srcDXGIFormat || desc.MipLevels != 1 || ArraySlice >= desc.ArraySize ||
		    desc.Width < m_srcWidth || desc.Height < m_srcHeight) {
			return E_UNEXPECTED;
		}

#if 0 // fix for issue #16. disable reinitialization code.
		if (desc.Width != m_srcWidth || desc.Height != m_srcHeight) {
			if (m_D3D11VP.IsReady()) {
				hr = InitializeD3D11VP(m_srcParams, desc.Width, desc.Height);
			} else {
				hr = InitializeTexVP(m_srcParams, desc.Width, desc.Height);
			}
			if (FAILED(hr)) {
				return hr;
			}
			UpdatFrameProperties();
			updateStats = true;
		}
#endif

		if (m_D3D11VP.IsReady()) {
			hr = m_D3D11VP.SetInputVideoData(pD3D11Texture2D, pSample, ArraySlice, m_SampleFormat);
		} else {
			// here should be used CopySubresourceRegion instead of CopyResource
			D3D11_BOX srcBox = { 0, 0, 0, m_srcWidth, m_srcHeight, 1 };
			m_pDeviceContext->CopySubresourceRegion(m_TexSrcVideo.pTexture, 0, 0, 0, 0, pD3D11Texture2D, ArraySlice, &srcBox);
		}
	}
	else {
		if (m_iSrcFromGPU != 0) {
			m_iSrcFromGPU = 0;
			updateStats = true;
		}

		BYTE* data = nullptr;
		const int size = pSample->GetActualDataLength();
		if (size < abs(m_srcPitch) * (int)m_srcLines) return VFW_E_BUFFER_UNDERFLOW;
		hr = pSample->GetPointer(&data);
		if (FAILED(hr)) return hr;
		if (!data) return E_POINTER;
		{
			// do not use UpdateSubresource for D3D11 VP here
			// because it can cause green screens and freezes on some configurations
			hr = MemCopyToTexSrcVideo(data, m_srcPitch);
			if (SUCCEEDED(hr) && !CanProjectJvr() && MfgFrameAnalysisEligible() && m_srcDXGIFormat == DXGI_FORMAT_NV12 &&
			    m_srcPitch > 0 && m_srcLines >= m_srcHeight + m_srcHeight / 2) {
				m_mfgDecodedDifference.SubmitCpuNv12(data, m_srcPitch, m_srcHeight, m_srcRect, m_rtStart, m_mfgLowMotionEnabled);
				comparedCpuNv12 = true;
			}
			if (SUCCEEDED(hr) && m_D3D11VP.IsReady()) {
				// ID3D11VideoProcessor does not use textures with D3D11_CPU_ACCESS_WRITE flag
				m_pDeviceContext->CopyResource(m_D3D11VP.GetNextInputTexture(m_SampleFormat), m_TexSrcVideo.pTexture);
			}
		}
	}

	if (!comparedCpuNv12 && !CanProjectJvr() && MfgFrameAnalysisEligible() && SUCCEEDED(hr)) {
		if (m_D3D11VP.IsReady()) {
			const auto held = m_D3D11VP.HoldFrame();
			m_mfgDecodedDifference.Submit(m_pDevice, m_pDeviceContext, held.pTexture,
				held.ArraySlice, m_srcRect, m_rtStart);
		} else {
			m_mfgDecodedDifference.Submit(m_pDevice, m_pDeviceContext, m_TexSrcVideo.pTexture,
				0, m_srcRect, m_rtStart);
		}
	}

	if (updateStats) {
		UpdateStatsStatic();
	}

	m_RenderStats.copyticks = GetPreciseTick() - tick;

	return hr;
}

HRESULT CDX11VideoProcessor::Render(int field, const REFERENCE_TIME frameStartTime)
{
	CheckPointer(m_TexSrcVideo.pTexture, E_FAIL);
	CheckPointer(m_pDXGISwapChain1, E_FAIL);

	const bool mfgConfigured = m_mfgMultiplier.load(std::memory_order_relaxed) >= 2;
	const bool mfgBypassed = mfgConfigured && MfgSourceMeetsDisplayRate();
	const bool mfgEnabledForRate = mfgConfigured && !mfgBypassed;
	if (field && mfgBypassed && m_mfgInputModeKnown) {
		// Scheduled input has drained legacy originals. Completed originals
		// already own their surfaces and retain their existing deadlines.
		StopMfg(false);
		if (!m_mfg.Failed()) m_mfg.ResetHistory();
		m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
		m_mfgInputModeKnown = false;
	}

	// Buffered video-only frames apply the window size at presentation.
	if (!m_completingSample) {
		if (field || !m_completedLast) {
			const auto resizeHr = ApplyPendingWindowResize();
			if (FAILED(resizeHr)) return resizeHr;
		}
		UpdateProcessingResolution();
	}
	if (!field && m_completedLast && (!CanProjectJvr() || m_pFilter->m_filterState == State_Running ||
	    m_completedLast->pts != m_rtStart)) {
		return PresentCompletedVideo(*m_completedLast);
	}
	if (field && !m_completingSample && !CompletedFramesPending()) m_completedLast.reset();
	const bool detached = m_processingSize.cx > 0;
	if (!field && detached && mfgEnabledForRate && m_mfgInputModeKnown &&
	    !m_mfg.Failed() && !m_mfgPresentFailed && m_pFilter->m_filterState == State_Running) {
		// The presenter owns the current picture and will draw with the new window geometry.
		return S_OK;
	}
	const bool completedVideoOnly = m_completedTarget && m_completedTarget->videoOnly;
	if (field) {
		m_FieldDrawn = field;
	}
	const bool second = m_FieldDrawn == 2;
	const bool wantsProjection = CanProjectJvr() && !m_windowRect.IsRectEmpty() && !m_videoRect.IsRectEmpty();
	const CRect sourceRect = ProcessingSourceRect(wantsProjection);
	const uint64_t tick1 = GetPreciseTick();
	const auto projectionHr = wantsProjection ? PrepareJvrFrame(sourceRect, second) : S_FALSE;
	const bool projected = wantsProjection && SUCCEEDED(projectionHr);
	const auto projectionTicks = wantsProjection ? GetPreciseTick() - tick1 : 0;
	DLogIf(FAILED(projectionHr), L"JVR source preparation failed ({}); showing decoded source", HR2Str(projectionHr));
	const bool sourceReady = !wantsProjection || projected;
	const CRect frameSource = projected ? sourceRect : m_srcRect;
	const CRect outputPicture = m_completedTarget ? m_completedTarget->picture : PictureForPlayback(projected);
	const CRect processingRect = detached ? ProcessingRect() : outputPicture;
	if (m_completedTarget) m_completedTarget->projected = projected;

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = S_OK;
	if (m_completedTarget) {
		pBackBuffer = m_completedTarget->image.pTexture;
		m_completedTarget->pts = frameStartTime;
		m_completedTarget->end = frameStartTime + m_sampleDuration;
		m_completedTarget->due = frameStartTime + m_rtVideoDelay;
	} else {
		hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::Render() : GetBuffer() failed with error {}", HR2Str(hr));
			return hr;
		}
	}

	if (!m_windowRect.IsRectEmpty()) {
		// fill the BackBuffer with black
		ID3D11RenderTargetView* pRenderTargetView;
		if (S_OK == m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView)) {
			const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			m_pDeviceContext->ClearRenderTargetView(pRenderTargetView, ClearColor);
			pRenderTargetView->Release();
		}
	}

	if (m_pDXGISwapChain4) {
		if (m_Dovi.bValid) {
			if (!m_hdr10.bValid && m_lastHdr10.bValid) {
				m_hdr10.bValid = true;
				m_hdr10.hdr10 = m_lastHdr10.hdr10;
			}

			if (m_hdr10.bValid) {
				if (m_DoviMaxMasteringLuminance > m_hdr10.hdr10.MaxMasteringLuminance) {
					m_hdr10.hdr10.MaxMasteringLuminance = m_DoviMaxMasteringLuminance;
				}
				if (m_DoviMinMasteringLuminance && m_DoviMinMasteringLuminance != m_hdr10.hdr10.MinMasteringLuminance) {
					m_hdr10.hdr10.MinMasteringLuminance = m_DoviMinMasteringLuminance;
				}
				if (m_DoviMaxContentLightLevel && m_DoviMaxContentLightLevel != m_hdr10.hdr10.MaxContentLightLevel) {
					m_hdr10.hdr10.MaxContentLightLevel = m_DoviMaxContentLightLevel;
				}
				if (m_DoviMaxFrameAverageLightLevel && m_DoviMaxFrameAverageLightLevel != m_hdr10.hdr10.MaxFrameAverageLightLevel) {
					m_hdr10.hdr10.MaxFrameAverageLightLevel = m_DoviMaxFrameAverageLightLevel;
				}
			}
		}

		const DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
		if (m_currentSwapChainColorSpace != colorSpace) {
			UINT colorSpaceSupport = 0;
			if (SUCCEEDED(m_pDXGISwapChain4->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport))
					&& (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) {
				hr = m_pDXGISwapChain4->SetColorSpace1(colorSpace);
				DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetColorSpace1() failed with error {}", HR2Str(hr));
				if (SUCCEEDED(hr)) {
					m_currentSwapChainColorSpace = colorSpace;

					if (!m_bVPUseRTXVideoHDR) {
						if (m_hdr10.bValid) {
							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_hdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(hdr) failed with error {}", HR2Str(hr));
							}

							m_lastHdr10 = m_hdr10;
							UpdateStatsStatic();
						} else if (m_lastHdr10.bValid) {
							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_lastHdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(lastHdr) failed with error {}", HR2Str(hr));
							}
						} else {
							m_lastHdr10.bValid = true;

							m_lastHdr10.hdr10.RedPrimary[0] = 34000; // Display P3 primaries
							m_lastHdr10.hdr10.RedPrimary[1] = 16000;
							m_lastHdr10.hdr10.GreenPrimary[0] = 13250;
							m_lastHdr10.hdr10.GreenPrimary[1] = 34500;
							m_lastHdr10.hdr10.BluePrimary[0] = 7500;
							m_lastHdr10.hdr10.BluePrimary[1] = 3000;
							m_lastHdr10.hdr10.WhitePoint[0] = 15635;
							m_lastHdr10.hdr10.WhitePoint[1] = 16450;
							m_lastHdr10.hdr10.MaxMasteringLuminance = m_DoviMaxMasteringLuminance ? m_DoviMaxMasteringLuminance : 1000; // 1000 nits
							m_lastHdr10.hdr10.MinMasteringLuminance = m_DoviMinMasteringLuminance ? m_DoviMinMasteringLuminance : 50;   // 0.005 nits
							if (m_DoviMaxContentLightLevel) {
								m_lastHdr10.hdr10.MaxContentLightLevel = m_DoviMaxContentLightLevel;
							}
							if (m_DoviMaxFrameAverageLightLevel) {
								m_lastHdr10.hdr10.MaxFrameAverageLightLevel = m_DoviMaxFrameAverageLightLevel;
							}

							if (m_bHdrPassthrough) {
								hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_lastHdr10.hdr10);
								DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(Display P3 standard) failed with error {}", HR2Str(hr));
							}

							UpdateStatsStatic();
						}
					}
				}
			}
		} else if (m_currentSwapChainColorSpace == colorSpace && m_hdr10.bValid) {
			if (memcmp(&m_hdr10.hdr10, &m_lastHdr10.hdr10, sizeof(m_hdr10.hdr10)) != 0) {
				if (m_bHdrPassthrough) {
					hr = m_pDXGISwapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &m_hdr10.hdr10);
					DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : SetHDRMetaData(hdr) failed with error {}", HR2Str(hr));
				}

				m_lastHdr10 = m_hdr10;
				UpdateStatsStatic();
			}
		}

		if (m_bHdrLocalToneMapping && m_currentSwapChainColorSpace == colorSpace) {
			if (m_DoviExtensionMetadata.L1.present) {
				SetHDR10ShaderParams(m_DoviExtensionMetadata.L1.min_pq, m_DoviExtensionMetadata.L1.max_pq,
									 m_DoviExtensionMetadata.L1.max_pq, m_DoviExtensionMetadata.L1.avg_pq,
									 m_iHdrDisplayMaxNits, HdrLocalToneMappingType());
			} else if (m_lastHdr10.bValid) {
				SetHDR10ShaderParams(m_lastHdr10.hdr10.MinMasteringLuminance, m_lastHdr10.hdr10.MaxMasteringLuminance,
									 m_lastHdr10.hdr10.MaxContentLightLevel, m_lastHdr10.hdr10.MaxFrameAverageLightLevel,
									 m_iHdrDisplayMaxNits, m_iHdrLocalToneMappingType);
			}
		}
	}

	const bool playFirst = !m_pFilter->m_Sets.bWaitForExtendedStartup;
	const bool wantSr = sourceReady && CanDlssSRUpscale(frameSource, processingRect, projected);
	if (projected && field && frameStartTime >= 0) {
		if (MfgFrameAnalysisEligible()) {
			m_mfgDecodedDifference.Submit(m_pDevice, m_pDeviceContext, m_jvrFrame.pTexture, 0, frameSource, frameStartTime);
		}
		if ((UsesZipDepthSr() && wantSr) || UsesZipDepthMfg()) {
			m_zipDepth.Update(m_pDevice, m_pDeviceContext, m_jvrFrame.pShaderResource,
				frameSource, frameSource.Size(), frameStartTime);
		}
	}
	if (m_svdLut.strength && sourceReady && field == 1) {
		CaptureSvdLut(frameStartTime, projected, second);
	}
	const bool warmZipDepth = (UsesZipDepthMfg() && MfgDepthSourceEligible()) ||
		(UsesZipDepthSr() && wantSr);
	if (field && frameStartTime != INVALID_TIME && warmZipDepth && (playFirst || mfgEnabledForRate)) {
		m_zipDepth.Prepare(m_pDevice);
	}
	const bool canEnhance = !playFirst || (m_startupHasPicture && (!warmZipDepth || !m_zipDepth.Loading()));
	const bool pausedMfg = sourceReady && mfgEnabledForRate && field && frameStartTime != INVALID_TIME &&
		m_pFilter->m_filterState == State_Paused && !m_pPSHalfOUtoInterlace &&
		!m_mfg.Failed();
	const bool prerollMfg = pausedMfg && !playFirst;
	const bool canQueueMfg = sourceReady && mfgEnabledForRate && field && frameStartTime != INVALID_TIME &&
		m_pFilter->m_filterState == State_Running && !m_pFilter->m_bClockStalled && !m_pPSHalfOUtoInterlace;
	const bool prepareMfg = canQueueMfg || pausedMfg;
	auto mfgReady = [&](ID3D11Texture2D* input) {
		return !playFirst || m_mfg.PollWarmup(m_pDevice, m_pDeviceContext, input);
	};
	bool useMfg = canQueueMfg && canEnhance;
	bool nativeWarmupSelected = false;
	bool nativeWarmupPending = false;
	m_extendedStartupPending = playFirst && (!canEnhance && (prepareMfg || wantSr));
	// Fast path is deliberately conservative.  D3D11 VP (or the original color
	// conversion shader) reconstructs chroma/CSC at native video dimensions.
	// MFG then interpolates that RGB image, and the regular resizer runs once
	// for *both* real and generated pictures in CompositeMfg().  Never bypass
	// RTX VSR/HDR, user post shaders, dithering, rotation or interlacing.
	// Auto x1 still needs the same source-first path for its original frame.
	// Switching to legacy merely because no interpolation is needed would flush
	// pending real frames and invert the source timeline.
    const bool postUpscaleRequested = prepareMfg && m_iVPSuperRes != SUPERRES_Disable && MfgPostUpscaleRequested();
    const bool postVsrCandidate = postUpscaleRequested && m_bVPUseSuperRes;
    const bool postDlssSRCandidate = postUpscaleRequested && CanDlssSRUpscale(frameSource, processingRect, projected);
    // Native-size DLAA can be evaluated after MFG without an enlargement.
    const bool postNativeDlaa = postDlssSRCandidate &&
        processingRect.Size() == frameSource.Size();
	const bool nativeMfg = prepareMfg && (!m_resizePending || detached) && !m_mfg.Failed() &&
		!m_mfgPresentFailed.load(std::memory_order_relaxed) && (completedVideoOnly || projected || !m_renderRect.IsRectEmpty()) &&
		!frameSource.IsRectEmpty() && (frameSource.Height() <= 1440 || postNativeDlaa) &&
		((processingRect.Width() > frameSource.Width() && processingRect.Height() > frameSource.Height()) || postNativeDlaa) &&
		((!m_bInterlaced && !m_bDoubleFrames && m_SampleFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) || postDlssSRCandidate) &&
		m_iRotation == 0 && !m_bFlip &&
		!SourceIsHDR() && !m_bVPRTXVideoHDR && !m_bVPUseRTXVideoHDR &&
        ((m_iVPSuperRes == SUPERRES_Disable && !m_bVPUseSuperRes) || postVsrCandidate || postDlssSRCandidate) &&
		GetPostScaleSteps() == 0 &&
		m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM &&
		(projected || (m_D3D11VP.IsReady() ? m_D3D11OutputFmt : m_InternalTexFmt) == DXGI_FORMAT_B8G8R8A8_UNORM);
	if (nativeMfg) {
		// No global preference changes: MPC-VR's normal VP/VSR settings remain
		// authoritative.  Set MPCVR_MFG_NATIVE_INPUT=0 to A/B the legacy path.
		static const bool nativeEnabled = [] {
			wchar_t setting[8]{};
			return GetEnvironmentVariableW(L"MPCVR_MFG_NATIVE_INPUT", setting, _countof(setting)) == 0 || setting[0] != L'0';
		}();
        // Post-MFG VSR is opt-in. A failed initialization MUST keep the
        // original VSR-before-MFG path, not silently use Shader scaling.
        auto& nativeInput = projected ? m_jvrFrame : m_mfgNativeInput;
        nativeWarmupSelected = nativeEnabled && (!postVsrCandidate || EnsureMfgPostVsr()) &&
            (projected || SUCCEEDED(nativeInput.CheckCreate(m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM,
                frameSource.Width(), frameSource.Height(), Tex2D_DefaultShaderRTarget)));
        if (nativeWarmupSelected) {
            nativeWarmupPending = !mfgReady(nativeInput.pTexture);
            m_extendedStartupPending |= nativeWarmupPending;
            if (postDlssSRCandidate && prerollMfg) {
                const CRect nativeRect(0, 0, frameSource.Width(), frameSource.Height());
                PrepareDlssSR(nativeRect, processingRect, true);
            }
        }
        if (nativeWarmupSelected &&
            !nativeWarmupPending && (prerollMfg || (useMfg && !m_mfg.Failed()))) {
            const CRect nativeRect(0, 0, frameSource.Width(), frameSource.Height());
            const auto nativeStart=int64_t(GetPreciseTick());
            HRESULT nativeHr=E_FAIL;
            if (projected) {
                nativeHr = S_OK;
            } else if (postVsrCandidate) {
                // Copy/CSC through a separate VP that has no RTX VSR enabled.
                // The regular decoder VP is never reconfigured.
                const auto held=m_D3D11VP.HoldFrame();
                if(held.pTexture) nativeHr=m_mfgPreVsrVP.ProcessExternal(
                    held.pTexture,held.ArraySlice,m_mfgNativeInput.pTexture,m_srcRect,nativeRect);
                if(FAILED(nativeHr)) {
                    m_mfgPostVsrBroken=true;
                    DLog(L"Hybrid MFG pre-VSR source conversion failed: {}; keeping Golden",HR2Str(nativeHr));
                }
            } else {
                nativeHr = m_D3D11VP.IsReady()
                    ? D3D11VPPass(m_mfgNativeInput.pTexture,m_srcRect,nativeRect,m_FieldDrawn==2)
                    : Process(m_mfgNativeInput.pTexture,m_srcRect,nativeRect,m_FieldDrawn==2);
            }
            const auto nativeEnd=int64_t(GetPreciseTick());
            if (nativeEnd > nativeStart)
                m_mfgVideoTicks.fetch_add(uint64_t(nativeEnd-nativeStart) + projectionTicks, std::memory_order_relaxed);
            m_mfgVideoCount.fetch_add(1,std::memory_order_relaxed);
            if (SUCCEEDED(nativeHr)) {
                if (prerollMfg) {
                    // Create the NGX feature/flow/banks while the DirectShow
                    // clock is still stopped. Never enqueue a preroll frame.
                    m_mfg.Warmup(m_pDevice, m_pDeviceContext, nativeInput.pTexture, 2);
                }
                bool nativeOverlayHandled = false;
                if (useMfg && QueueMfg(nativeInput.pTexture, frameStartTime, nativeOverlayHandled, true, postVsrCandidate, false, projected)) {
                    m_bVPScalingUseShaders=m_D3D11VP.IsReady() && !postVsrCandidate;
                    return S_OK;
                }
            } else {
                nativeWarmupSelected = false;
            }
        }
	}

	bool videoOnly = false;
	bool overlayInputHandled = false;
	// Redraws keep the current PTS. The projected preview uses the newly
	// sampled surface directly instead of SR's cached result for that PTS.
	const auto timestamp = frameStartTime == INVALID_TIME
		? m_rtStart + (second ? m_sampleDuration / 2 : 0)
		: frameStartTime;
	auto processPicture = [&](ID3D11Texture2D* target, const CRect& rect, CTex2DRing* postScale) {
		const bool canWarm = !nativeWarmupSelected && prepareMfg && (!m_resizePending || postScale);
		const bool ready = !nativeWarmupPending && (!canWarm || mfgReady(target));
		m_extendedStartupPending |= playFirst && !ready;
		useMfg = !nativeWarmupSelected && canQueueMfg && canEnhance && ready;
		const bool srAfterNativeMfg = nativeWarmupSelected && postDlssSRCandidate &&
			(prerollMfg || (playFirst && !m_mfg.Failed()));
		const bool allowSr = sourceReady && canEnhance && ready && !srAfterNativeMfg && (field || !projected);
		const auto result = Process(target, frameSource, rect, second, allowSr, timestamp, postScale,
			&overlayInputHandled, projected ? &m_jvrFrame : nullptr);
		m_extendedStartupPending |= playFirst && m_dlssSR.Preparing();
		return result;
	};
	if (completedVideoOnly || projected || !m_renderRect.IsRectEmpty()) {
		const auto videoStart=int64_t(GetPreciseTick());
		if (completedVideoOnly) {
			hr = processPicture(pBackBuffer, outputPicture, &m_processingPostScale);
			if (FAILED(hr)) return hr;
		} else if (detached && SUCCEEDED(m_processingTexture.CheckCreate(m_pDevice, m_SwapChainFmt,
			processingRect.Width(), processingRect.Height(), Tex2D_DefaultShaderRTarget))) {
			const auto steps = GetPostScaleSteps();
			if (SUCCEEDED(m_processingPostScale.CheckCreate(m_pDevice, m_InternalTexFmt,
				processingRect.Width(), processingRect.Height(), steps))) {
				hr = processPicture(m_processingTexture.pTexture, processingRect, &m_processingPostScale);
				videoOnly = SUCCEEDED(hr);
			}
		}
		if (!completedVideoOnly && !videoOnly) {
			hr = processPicture(pBackBuffer, outputPicture, nullptr);
			if (FAILED(hr)) return hr;
		}
		if (mfgEnabledForRate) {
			const auto videoEnd=int64_t(GetPreciseTick());
			if (videoEnd > videoStart)
				m_mfgVideoTicks.fetch_add(uint64_t(videoEnd-videoStart) + projectionTicks, std::memory_order_relaxed);
			m_mfgVideoCount.fetch_add(1,std::memory_order_relaxed);
		}
	}

	// Legacy pre-MFG path: preroll the same full/display-sized input that
	// the running producer will use, without starting its presentation thread.
	const bool videoOnlyInput = videoOnly || completedVideoOnly;
	if (prerollMfg && (!m_resizePending || videoOnlyInput) &&
	    !m_mfg.InputSize().cx) {
		auto* mfgInput = videoOnly ? m_processingTexture.pTexture.p : pBackBuffer.p;
		if (mfgInput) m_mfg.Warmup(m_pDevice, m_pDeviceContext, mfgInput,
		    2);
	}
	// Legacy path preserves VSR/HDR and all other native renderer features.
	// A back-buffer-sized input may need to fall back while its resize is pending.
	auto* pictureTexture = videoOnly ? m_processingTexture.pTexture.p : pBackBuffer.p;
	if (useMfg && (!m_resizePending || videoOnlyInput)) {
		if (QueueMfg(pictureTexture, frameStartTime, overlayInputHandled, false, false, videoOnlyInput, projected)) return S_OK;
	} else if (sourceReady && mfgEnabledForRate && !m_resizePending && m_pFilter->m_filterState == State_Running) StopMfg(false);
	if (!m_completedTarget && (videoOnly || projected || !m_renderRect.IsRectEmpty())) {
		DrawSvdLut(pictureTexture, videoOnly ? processingRect : outputPicture, timestamp);
	}
	if (projected && DepthOverlayEnabled() && !overlayInputHandled) {
		DrawDepthOverlay(pictureTexture, videoOnlyInput ? processingRect : outputPicture, timestamp);
	} else if (!wantsProjection && !overlayInputHandled && (videoOnly || m_renderRect == m_videoRect)) {
		BlendSourceDepthOverlay(pictureTexture, videoOnly ? m_processingTexture.pShaderResource.p : nullptr,
			videoOnly ? processingRect : outputPicture, timestamp);
	}
	if (videoOnly) {
		hr = ResizeShaderPass(m_processingTexture, pBackBuffer, processingRect, outputPicture, 0);
	}
	if (m_completingSample && m_completedTarget) {
		if (FAILED(hr)) return hr;
		if (!m_completedBatches.empty()) {
			m_completedBatches.push_back({std::move(*m_completedTarget), {}});
			m_completedPresenter.Wake();
			m_startupHasPicture = true;
			return S_OK;
		}
		if (!QueueCompletedFrame(std::move(*m_completedTarget))) return E_FAIL;
		m_pDeviceContext->Flush();
		m_startupHasPicture = true;
		return S_OK;
	}
	if (!m_pPSHalfOUtoInterlace) {
		DrawSubtitles(pBackBuffer, projected ? &outputPicture : nullptr);
	}

	if (m_bAlphaBitmapEnable) {
		D3D11_TEXTURE2D_DESC desc;
		pBackBuffer->GetDesc(&desc);
		D3D11_VIEWPORT VP = {
			m_AlphaBitmapNRectDest.left * desc.Width,
			m_AlphaBitmapNRectDest.top * desc.Height,
			(m_AlphaBitmapNRectDest.right - m_AlphaBitmapNRectDest.left) * desc.Width,
			(m_AlphaBitmapNRectDest.bottom - m_AlphaBitmapNRectDest.top) * desc.Height,
			0.0f,
			1.0f
		};
		hr = AlphaBlt(m_TexAlphaBitmap.pShaderResource, pBackBuffer,
			m_pAlphaBitmapVertex, &VP,
			m_pSamplerLinear);
	}

#if 0
	{ // Tearing test (very non-optimal implementation, use only for tests)
		static int nTearingPos = 0;

		ID3D11RenderTargetView* pRenderTargetView;
		if (S_OK == m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView)) {
			CD3D11Rectangle d3d11rect;
			HRESULT hr2 = d3d11rect.InitDeviceObjects(m_pDevice, m_pDeviceContext);

			const SIZE szWindow = m_windowRect.Size();
			RECT rcTearing;

			rcTearing.left = nTearingPos;
			rcTearing.top = 0;
			rcTearing.right = rcTearing.left + 4;
			rcTearing.bottom = szWindow.cy;
			hr2 = d3d11rect.Set(rcTearing, szWindow, D3DCOLOR_XRGB(255, 0, 0));
			hr2 = d3d11rect.Draw(pRenderTargetView, szWindow);

			rcTearing.left = (rcTearing.right + 15) % szWindow.cx;
			rcTearing.right = rcTearing.left + 4;
			hr2 = d3d11rect.Set(rcTearing, szWindow, D3DCOLOR_XRGB(255, 0, 0));
			hr2 = d3d11rect.Draw(pRenderTargetView, szWindow);

			pRenderTargetView->Release();
			d3d11rect.InvalidateDeviceObjects();

			nTearingPos = (nTearingPos + 7) % szWindow.cx;
		}
	}
#endif

	const LONG featureOffset = DrawFeatureStatus(pBackBuffer);
	if (m_bShowStats) DrawStats(pBackBuffer, featureOffset);
	uint64_t tick3 = GetPreciseTick();
	m_RenderStats.paintticks = tick3 - tick1;

	if (m_bVBlankBeforePresent && m_pDXGIOutput) {
		hr = m_pDXGIOutput->WaitForVBlank();
		DLogIf(FAILED(hr), L"WaitForVBlank failed with error {}", HR2Str(hr));
	}

	if (m_bAdjustPresentTime) {
		SyncFrameToStreamTime(frameStartTime==INVALID_TIME?INVALID_TIME:frameStartTime+m_rtVideoDelay);
	}

	g_bPresent = true;
	hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	if (SUCCEEDED(hr)) {
		if (field) m_startupHasPicture = true;
		m_pFilter->OnVideoPresented();
	}
	// If a frame bypassed MFG (warmup/fallback), it was absent from MFG's
	// presenter counters even though the normal renderer submitted it.
	if (hr==S_OK && mfgEnabledForRate)
		m_mfgFallbackPresents.fetch_add(1,std::memory_order_relaxed);
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::Render() : Present() failed with error {}", HR2Str(hr));

	m_RenderStats.presentticks = GetPreciseTick() - tick3;

	if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen) {
		InitSwapChain(false);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::FillBlack()
{
	CheckPointer(m_pDXGISwapChain1, E_ABORT);

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FillBlack() : GetBuffer() failed with error {}", HR2Str(hr));
		return hr;
	}

	ID3D11RenderTargetView* pRenderTargetView;
	hr = m_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FillBlack() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	const FLOAT ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	m_pDeviceContext->ClearRenderTargetView(pRenderTargetView, ClearColor);
	pRenderTargetView->Release();

	if (m_bShowStats) {
		hr = DrawStats(pBackBuffer);
	}

	if (m_bAlphaBitmapEnable) {
		D3D11_TEXTURE2D_DESC desc;
		pBackBuffer->GetDesc(&desc);
		D3D11_VIEWPORT VP = {
			m_AlphaBitmapNRectDest.left * desc.Width,
			m_AlphaBitmapNRectDest.top * desc.Height,
			(m_AlphaBitmapNRectDest.right - m_AlphaBitmapNRectDest.left) * desc.Width,
			(m_AlphaBitmapNRectDest.bottom - m_AlphaBitmapNRectDest.top) * desc.Height,
			0.0f,
			1.0f
		};
		hr = AlphaBlt(m_TexAlphaBitmap.pShaderResource, pBackBuffer,
			m_pAlphaBitmapVertex, &VP,
			m_pSamplerLinear);
	}

	g_bPresent = true;
	hr = m_pDXGISwapChain1->Present(1, 0);
	g_bPresent = false;
	DLogIf(FAILED(hr), L"CDX11VideoProcessor::FillBlack() : Present() failed with error {}", HR2Str(hr));

	if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen) {
		InitSwapChain(false);
	}

	return hr;
}

void CDX11VideoProcessor::UpdateTexures()
{
	if (!m_srcWidth || !m_srcHeight) {
		return;
	}

	// TODO: try making w and h a multiple of 128.
	HRESULT hr = S_OK;

	if (m_D3D11VP.IsReady()) {
		if (m_bVPScaling) {
			// Keep VP scaling on the same canvas as the extended processing pass.
			const CSize texsize = m_processingSize.cx > 0 ? m_processingSize : m_videoRect.Size();
			hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, texsize.cx, texsize.cy, Tex2D_DefaultShaderRTarget);
			if (FAILED(hr)) {
				hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
			}
		} else {
			hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_D3D11OutputFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
		}
	}
	else {
		hr = m_TexConvertOutput.CheckCreate(m_pDevice, m_InternalTexFmt, m_srcRectWidth, m_srcRectHeight, Tex2D_DefaultShaderRTarget);
	}
}

void CDX11VideoProcessor::UpdatePostScaleTexures()
{
	const bool needDither =
		(m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM && m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM
		|| m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM && m_InternalTexFmt == DXGI_FORMAT_R16G16B16A16_FLOAT);

	m_bFinalPass = (m_bUseDither && needDither && m_TexDither.pTexture);
	if (m_bFinalPass) {
		m_pPSFinalPass.Release();
		m_bFinalPass = SUCCEEDED(CreatePShaderFromResource(
			&m_pPSFinalPass,
			(m_SwapChainFmt == DXGI_FORMAT_R10G10B10A2_UNORM) ? IDF_PS_11_FINAL_PASS_10 : IDF_PS_11_FINAL_PASS
		));
	}

	const UINT numPostScaleSteps = GetPostScaleSteps();
	HRESULT hr = m_TexsPostScale.CheckCreate(m_pDevice, m_InternalTexFmt, m_windowRect.Width(), m_windowRect.Height(), numPostScaleSteps);
	//UpdateStatsPostProc();
}

void CDX11VideoProcessor::UpdateUpscalingShaders()
{
	m_pShaderUpscaleX.Release();
	m_pShaderUpscaleY.Release();

	if (m_iUpscaling != UPSCALE_Nearest) {
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderUpscaleX, s_Upscaling11ResIDs[m_iUpscaling].shaderX));
		if (m_iUpscaling == UPSCALE_Jinc2) {
			m_pShaderUpscaleY = m_pShaderUpscaleX;
		} else {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderUpscaleY, s_Upscaling11ResIDs[m_iUpscaling].shaderY));
		}
	}

	UpdateScalingStrings();
}

void CDX11VideoProcessor::UpdateDownscalingShaders()
{
	m_pShaderDownscaleX.Release();
	m_pShaderDownscaleY.Release();

	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderDownscaleX, s_Downscaling11ResIDs[m_iDownscaling].shaderX));
	EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pShaderDownscaleY, s_Downscaling11ResIDs[m_iDownscaling].shaderY));

	UpdateScalingStrings();
}

HRESULT CDX11VideoProcessor::UpdateConvertColorShader()
{
	m_pPSConvertColor.Release();
	m_pPSConvertColorDeint.Release();
	ID3DBlob* pShaderCode = nullptr;

	int convertType = (m_bConvertToSdr && !(m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping))) ? SHADER_CONVERT_TO_SDR
		: (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) ? SHADER_CONVERT_TO_PQ
		: SHADER_CONVERT_NONE;

	MediaSideDataDOVIMetadata* pDOVIMetadata = m_Dovi.bValid ? &m_Dovi.msd : nullptr;

	HRESULT hr = GetShaderConvertColor(true,
		m_srcWidth,
		m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
		m_srcRect, m_srcParams, m_srcExFmt, pDOVIMetadata,
		m_iChromaScaling, convertType, false, m_bSdrToneMapping,
		&pShaderCode);
	if (S_OK == hr) {
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPSConvertColor);
		pShaderCode->Release();
	}

	if (m_bInterlaced && m_srcParams.Subsampling == 420 && m_srcParams.pDX11Planes) {
		hr = GetShaderConvertColor(true,
			m_srcWidth,
			m_TexSrcVideo.desc.Width, m_TexSrcVideo.desc.Height,
			m_srcRect, m_srcParams, m_srcExFmt, pDOVIMetadata,
			m_iChromaScaling, convertType, true, m_bSdrToneMapping,
			&pShaderCode);
		if (S_OK == hr) {
			hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPSConvertColorDeint);
			pShaderCode->Release();
		}
	}

	if (FAILED(hr)) {
		ASSERT(0);
		UINT resid = 0;
		if (m_srcParams.cformat == CF_YUY2) {
			resid = IDF_PS_11_CONVERT_YUY2;
		}
		else if (m_srcParams.pDX11Planes) {
			if (m_srcParams.pDX11Planes->FmtPlane3) {
				if (m_srcParams.cformat == CF_YV12 || m_srcParams.cformat == CF_YV16 || m_srcParams.cformat == CF_YV24) {
					resid = IDF_PS_11_CONVERT_PLANAR_YV;
				} else {
					resid = IDF_PS_11_CONVERT_PLANAR;
				}
			} else {
				resid = IDF_PS_11_CONVERT_BIPLANAR;
			}
		}
		else {
			resid = IDF_PS_11_CONVERT_COLOR;
		}
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSConvertColor, resid));

		return S_FALSE;
	}

	return hr;
}

void CDX11VideoProcessor::UpdateBitmapShader()
{
	if (m_bPGSSubtitles && SourceIsHDR10()) {
		if (m_bHdrDisplayModeEnabled) {
			float SDR_peak_lum;
			switch (m_iHdrOsdBrightness) {
				default:
					SDR_peak_lum = 100;
					break;
				case 1:
					SDR_peak_lum = 50;
					break;
				case 2:
					SDR_peak_lum = 30;
					break;
			}

			m_pPS_BitmapToFrame = m_pPS_Simple;
			m_dwStatsTextColor = TransferPQ(D3DCOLOR_XRGB(255, 255, 255), SDR_peak_lum);
		} else {
			if (m_bConvertToSdr) {
				m_pPS_BitmapToFrame.Release();
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_BitmapToFrame, IDF_PS_11_CONVERT_PQ_TO_SDR));
			} else {
				m_pPS_BitmapToFrame = m_pPS_Simple;
			}
			m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);
		}

		return;
	}

	if (m_bHdrDisplayModeEnabled
			&& (SourceIsHDR() || m_bVPUseRTXVideoHDR)) {
		UINT resid;
		float SDR_peak_lum;
		switch (m_iHdrOsdBrightness) {
		default:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ;
			SDR_peak_lum = 100;
			break;
		case 1:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ1;
			SDR_peak_lum = 50;
			break;
		case 2:
			resid = IDF_PS_11_CONVERT_BITMAP_TO_PQ2;
			SDR_peak_lum = 30;
			break;
		}
		m_pPS_BitmapToFrame.Release();
		EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_BitmapToFrame, resid));
		m_dwStatsTextColor = TransferPQ(D3DCOLOR_XRGB(255, 255, 255), SDR_peak_lum);
	}
	else {
		m_pPS_BitmapToFrame = m_pPS_Simple;
		m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);
	}
}

HRESULT CDX11VideoProcessor::D3D11VPPass(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second)
{
	HRESULT hr = m_D3D11VP.SetRectangles(srcRect, dstRect);
	if (FAILED(hr)) return hr;

	hr = m_D3D11VP.Process(pRenderTarget, m_SampleFormat, second);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::ProcessD3D11() : m_D3D11VP.Process() failed with error {}", HR2Str(hr));
	}

	return hr;
}

HRESULT CDX11VideoProcessor::ConvertColorPass(ID3D11Texture2D* pRenderTarget)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"ConvertColorPass() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	D3D11_VIEWPORT VP;
	VP.TopLeftX = 0;
	VP.TopLeftY = 0;
	VP.Width = (FLOAT)m_TexConvertOutput.desc.Width;
	VP.Height = (FLOAT)m_TexConvertOutput.desc.Height;
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	const UINT Stride = sizeof(VERTEX);
	const UINT Offset = 0;

	// Set resources
	m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
	m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView.p, nullptr);
	m_pDeviceContext->RSSetViewports(1, &VP);
	m_pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
	if (m_bDeintBlend && m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE && m_pPSConvertColorDeint) {
		m_pDeviceContext->PSSetShader(m_pPSConvertColorDeint, nullptr, 0);
	} else {
		m_pDeviceContext->PSSetShader(m_pPSConvertColor, nullptr, 0);
	}
	m_pDeviceContext->PSSetShaderResources(0, 1, &m_TexSrcVideo.pShaderResource.p);
	m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexSrcVideo.pShaderResource2.p);
	m_pDeviceContext->PSSetShaderResources(2, 1, &m_TexSrcVideo.pShaderResource3.p);
	m_pDeviceContext->PSSetSamplers(0, 1, &m_pSamplerPoint.p);
	m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerLinear.p);
	m_pDeviceContext->PSSetConstantBuffers(0, 1, &m_PSConvColorData.pConstants);
	m_pDeviceContext->PSSetConstantBuffers(1, 1, &m_pCorrectionConstants.p);
	m_pDeviceContext->PSSetConstantBuffers(2, 1, &m_pDoviCurvesConstantBuffer.p);
	if (m_pDoViDynamicConstants) {
		m_pDeviceContext->PSSetConstantBuffers(3, 1, &m_pDoViDynamicConstants.p);
	}
	m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_pDeviceContext->IASetVertexBuffers(0, 1, &m_PSConvColorData.pVertexBuffer, &Stride, &Offset);

	// Draw textured quad onto render target
	m_pDeviceContext->Draw(4, 0);

	ID3D11ShaderResourceView* views[3] = {};
	m_pDeviceContext->PSSetShaderResources(0, 3, views);

	return hr;
}

HRESULT CDX11VideoProcessor::ResizeShaderPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget,
	const CRect& srcRect, const CRect& dstRect, const int rotation, bool applyFlip, Tex2D_t* intermediate)
{
	HRESULT hr = S_OK;
	const bool flip = applyFlip && m_bFlip;
	const int w2 = dstRect.Width();
	const int h2 = dstRect.Height();
	const int k = m_bInterpolateAt50pct ? 2 : 1;

	int w1, h1;
	ID3D11PixelShader* resizerX;
	ID3D11PixelShader* resizerY;
	if (rotation == 90 || rotation == 270) {
		w1 = srcRect.Height();
		h1 = srcRect.Width();
		resizerX = (w1 == w2) ? nullptr : (w1 > k * w2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p; // use Y scaling here
		if (resizerX) {
			resizerY = (h1 == h2) ? nullptr : (h1 > k * h2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p;
		} else {
			resizerY = (h1 == h2) ? nullptr : (h1 > k * h2) ? m_pShaderDownscaleX.p : m_pShaderUpscaleX.p; // use X scaling here
		}
	} else {
		w1 = srcRect.Width();
		h1 = srcRect.Height();
		resizerX = (w1 == w2) ? nullptr : (w1 > k * w2) ? m_pShaderDownscaleX.p : m_pShaderUpscaleX.p;
		resizerY = (h1 == h2) ? nullptr : (h1 > k * h2) ? m_pShaderDownscaleY.p : m_pShaderUpscaleY.p;
	}

	if (resizerX && resizerY) {
		// two pass resize

		D3D11_TEXTURE2D_DESC desc;
		pRenderTarget->GetDesc(&desc);

		if (resizerX == resizerY) {
			// one pass resize
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerX, rotation, flip);
			DLogIf(FAILED(hr), L"CDX11VideoProcessor::ResizeShaderPass() : failed with error {}", HR2Str(hr));

			return hr;
		}

		// check intermediate texture
		const UINT texWidth  = desc.Width;
		const UINT texHeight = h1;

		auto& temp = intermediate ? *intermediate : m_TexResize;
		hr = temp.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, texWidth, texHeight, Tex2D_DefaultShaderRTarget);
		if (FAILED(hr)) {
			DLog(L"CDX11VideoProcessor::ResizeShaderPass() : intermediate texture failed with error {}", HR2Str(hr));
			return hr;
		}

		CRect resizeRect(dstRect.left, 0, dstRect.right, texHeight);

		// First resize pass
		hr = TextureResizeShader(Tex, temp.pTexture, srcRect, resizeRect, resizerX, rotation, flip);
		if (SUCCEEDED(hr)) {
			hr = TextureResizeShader(temp, pRenderTarget, resizeRect, dstRect, resizerY, 0, false);
		}
	}
	else {
		if (resizerX) {
			// one pass resize for width
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerX, rotation, flip);
		}
		else if (resizerY) {
			// one pass resize for height
			hr = TextureResizeShader(Tex, pRenderTarget, srcRect, dstRect, resizerY, rotation, flip);
		}
		else {
			// no resize
			hr = TextureCopyRect(Tex, pRenderTarget, srcRect, dstRect, m_pPS_Simple, nullptr, rotation, flip);
		}
	}

	DLogIf(FAILED(hr), L"CDX11VideoProcessor::ResizeShaderPass() : failed with error {}", HR2Str(hr));

	return hr;
}

HRESULT CDX11VideoProcessor::FinalPass(const Tex2D_t& Tex, ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect)
{
	CComPtr<ID3D11RenderTargetView> pRenderTargetView;

	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FinalPass() : CreateRenderTargetView() failed with error {}", HR2Str(hr));
		return hr;
	}

	hr = FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, Tex.desc.Width, Tex.desc.Height, srcRect, 0, false);
	if (FAILED(hr)) {
		return hr;
	}

	const FLOAT constants[4] = { (float)Tex.desc.Width / dither_size, (float)Tex.desc.Height / dither_size, 0, 0 };
	D3D11_MAPPED_SUBRESOURCE mr;
	hr = m_pDeviceContext->Map(m_pFinalPassConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::FinalPass() : Map() failed with error {}", HR2Str(hr));
		return hr;
	}

	memcpy(mr.pData, &constants, sizeof(constants));
	m_pDeviceContext->Unmap(m_pFinalPassConstantBuffer, 0);

	D3D11_VIEWPORT VP;
	VP.TopLeftX = (FLOAT)dstRect.left;
	VP.TopLeftY = (FLOAT)dstRect.top;
	VP.Width    = (FLOAT)dstRect.Width();
	VP.Height   = (FLOAT)dstRect.Height();
	VP.MinDepth = 0.0f;
	VP.MaxDepth = 1.0f;

	// Set resources
	m_pDeviceContext->PSSetShaderResources(1, 1, &m_TexDither.pShaderResource.p);
	m_pDeviceContext->PSSetSamplers(1, 1, &m_pSamplerDither.p);

	TextureBlt11(m_pDeviceContext, pRenderTargetView, VP, m_pVSimpleInputLayout, m_pVS_Simple, m_pPSFinalPass, Tex.pShaderResource, m_pSamplerPoint, m_pFinalPassConstantBuffer, m_pVertexBuffer);

	ID3D11ShaderResourceView* views[1] = {};
	m_pDeviceContext->PSSetShaderResources(1, 1, views);

	return hr;
}

bool CDX11VideoProcessor::DepthOverlayEnabled() const
{
	return m_showDepthOverlay && (UsesZipDepthSr() || UsesZipDepthMfg()) && !m_depthOverlayShaderFailed &&
		!m_bHdrDisplayModeEnabled && !m_pPSHalfOUtoInterlace &&
		MfgDepthSourceEligible();
}

bool CDX11VideoProcessor::BlendSourceDepthOverlay(ID3D11Texture2D* image, ID3D11ShaderResourceView* source,
	const CRect& picture, REFERENCE_TIME timestamp)
{
	if (!DepthOverlayEnabled() || picture.IsRectEmpty() || timestamp < 0) return false;

	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	// Swap-chain buffers need a shader-readable copy for depth capture.
	if (!source) {
		D3D11_TEXTURE2D_DESC desc{};
		image->GetDesc(&desc);
		if (FAILED(m_depthOverlayCapture.CheckCreate(m_pDevice, desc.Format,
			desc.Width, desc.Height, Tex2D_DefaultShaderRTarget))) return false;
		m_pDeviceContext->CopyResource(m_depthOverlayCapture.pTexture, image);
		source = m_depthOverlayCapture.pShaderResource;
	}
	const SIZE modelSize{LONG(m_srcRectWidth), LONG(m_srcRectHeight)};
	m_zipDepth.Update(m_pDevice, m_pDeviceContext, source, picture, modelSize, timestamp);
	DrawDepthOverlay(image, picture, timestamp);
	return true;
}

void CDX11VideoProcessor::DrawMfgMotion(ID3D11Texture2D* target, uint64_t serial, REFERENCE_TIME pts, const CRect& picture)
{
	const auto motion = m_mfg.OverlayMotion(serial, pts);
	if (!motion.view || picture.IsRectEmpty()) return;
	if (!m_pMfgMotionConstants) {
		static constexpr char source[] = R"(
Texture2D<float2> Motion : register(t0);
Texture2D<int4> Active : register(t1);
cbuffer Settings : register(b0) {
    float4 WorkPicture; // origin and extent in MFG pixels
    float2 OutputSize;
    float2 Padding;
};
float DistanceToLine(float2 p, float2 a, float2 b) {
    float2 d = b - a;
    return length(p - a - d * saturate(dot(p - a, d) / max(dot(d, d), 0.001)));
}
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    const float spacing = 32.0;
    float2 pixel = uv * OutputSize;
    float2 anchor = (floor(pixel / spacing) + 0.5) * spacing;
    if (any(anchor >= OutputSize)) return 0;
    int2 work = int2(WorkPicture.xy + anchor / OutputSize * WorkPicture.zw);
    const int4 bounds = Active.Load(int3(0, 0, 0));
    const float2 fragment = WorkPicture.xy + uv * WorkPicture.zw;
    if (any(work < bounds.xy) || any(work >= bounds.zw) ||
        any(fragment < bounds.xy) || any(fragment >= bounds.zw)) return 0;
    float2 mv = Motion.Load(int3(work, 0));
    float2 delta = mv * OutputSize / WorkPicture.zw;
    float len = length(delta);
    float2 p = pixel - anchor;
    float distance = length(p);
    if (len > 0.25) {
        float2 direction = delta / len;
        float2 tip = direction * clamp(len, 4.0, 13.0);
        float2 tail = tip - 3.0 * direction;
        float2 side = float2(-direction.y, direction.x) * 2.2;
        distance = min(DistanceToLine(p, 0, tip),
            min(DistanceToLine(p, tip, tail + side), DistanceToLine(p, tip, tail - side)));
    }
    return float4(0.15, 1.0, 0.55, 0.85 * saturate(1.65 - distance));
}
)";
		CComPtr<ID3DBlob> blob;
		CComPtr<ID3D11PixelShader> shader;
		CComPtr<ID3D11BlendState> blendState;
		D3D11_BLEND_DESC blend{};
		auto& rt = blend.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		const D3D11_BUFFER_DESC constants{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
		if (FAILED(CompileShader(source, nullptr, "ps_5_0", &blob)) ||
			FAILED(m_pDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader)) ||
			FAILED(m_pDevice->CreateBlendState(&blend, &blendState)) ||
			FAILED(m_pDevice->CreateBuffer(&constants, nullptr, &m_pMfgMotionConstants))) return;
		m_pMfgMotionShader = shader;
		m_pMfgMotionBlend = blendState;
	}

	const CRect work(motion.picture);
	const float constants[8]{float(work.left), float(work.top), float(work.Width()), float(work.Height()),
		float(picture.Width()), float(picture.Height())};
	CComPtr<ID3D11RenderTargetView> view;
	if (FAILED(m_pDevice->CreateRenderTargetView(target, nullptr, &view)) ||
		FAILED(FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, 1, 1, CRect(0, 0, 1, 1), 0, false))) return;
	m_pDeviceContext->UpdateSubresource(m_pMfgMotionConstants, 0, nullptr, constants, 0, 0);
	m_pDeviceContext->RSSetState(nullptr);
	D3D11_VIEWPORT viewport{float(picture.left), float(picture.top), constants[4], constants[5], 0, 1};
	m_pDeviceContext->PSSetShaderResources(1, 1, &motion.active);
	TextureBlt11(m_pDeviceContext, view, viewport, m_pVSimpleInputLayout, m_pVS_Simple,
		m_pMfgMotionShader, motion.view, nullptr, m_pMfgMotionConstants, m_pVertexBuffer, m_pMfgMotionBlend);
	ID3D11ShaderResourceView* empty = nullptr;
	m_pDeviceContext->PSSetShaderResources(1, 1, &empty);
	m_pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	m_mfg.MarkConsumed(serial);
}

bool CDX11VideoProcessor::EnsureDepth3DMeshResources()
{
	if (m_depth3DPreviewShaderFailed) return false;
	if (m_pVS_Depth3DPreview && m_pPS_Depth3DPreview && m_pVDepth3DPreviewInputLayout &&
		m_pDepth3DPreviewConstants && m_pDepth3DPreviewVertexBuffer && m_pDepth3DPreviewIndexBuffer &&
		m_pDepth3DPreviewRasterizer && m_depth3DPreviewIndexCount) return true;

	if (!m_pVS_Depth3DPreview || !m_pPS_Depth3DPreview || !m_pVDepth3DPreviewInputLayout || !m_pDepth3DPreviewConstants) {
		static constexpr char vertexShaderSource[] = R"(
Texture2D<float> Depth : register(t0);
SamplerState Linear : register(s0);
cbuffer Settings : register(b0) {
    float4 SourceUV; // xy origin, zw extent in the completed video texture
    float4 Params;   // rotated, Z depth strength, normalized depth span, camera distance
};
struct VS_INPUT {
    float2 Pos : POSITION;
    float2 Tex : TEXCOORD0;
    float2 DepthCoord : TEXCOORD1;
};
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};
VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    float2 local = input.Pos;
    float2 depthCoord = input.DepthCoord;
    float2 depthUV = Params.x > 0.5 ? float2(1.0 - depthCoord.y, depthCoord.x) : depthCoord;
    float depth = Depth.SampleLevel(Linear, saturate(depthUV), 0);
    float height = clamp((0.5 - depth) / max(Params.z, 1e-4), -0.5, 0.5);

    // Each tile is independent. All four vertices in the tile share one sampled
    // depth value, so depth discontinuities create step changes rather than the
    // rubber-sheet tearing seen with a shared continuous mesh.
    float x = (local.x - 0.5) * 2.0;
    float y = (0.5 - local.y) * 2.0;
    float z = height * Params.y;
    const float c = 0.8660254; // cos(30 deg)
    const float s = 0.5;       // sin(30 deg)
    float xr = x * c + z * s;
    float zr = -x * s + z * c;

    // Small perspective projection. Fit keeps intentional black borders so
    // the whole tilted relief remains visible for this diagnostic preview.
    float cameraDistance = max(Params.w, 1.5);
    float perspective = cameraDistance / max(cameraDistance - zr, 0.5);
    const float fit = 0.80;
    output.Pos = float4(xr * perspective * fit, y * perspective * fit, 0.0, 1.0);
    output.Tex = local;
    return output;
}
)";
		static constexpr char pixelShaderSource[] = R"(
Texture2D<float4> Image : register(t1);
SamplerState Linear : register(s0);
cbuffer Settings : register(b0) {
    float4 SourceUV; // xy origin, zw extent in the completed video texture
    float4 Params;   // rotated, depth strength, normalized depth span, projected width scale
};
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};
float4 main(VS_OUTPUT input) : SV_TARGET {
    float2 sampleLocal = saturate(input.Tex);
    return Image.SampleLevel(Linear, SourceUV.xy + sampleLocal * SourceUV.zw, 0);
}
)";
		CComPtr<ID3DBlob> vsCode, psCode;
		D3D11_BUFFER_DESC constants{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
		if (FAILED(CompileShader(vertexShaderSource, nullptr, "vs_4_0", &vsCode)) ||
			FAILED(CompileShader(pixelShaderSource, nullptr, "ps_4_0", &psCode)) ||
			FAILED(m_pDevice->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &m_pVS_Depth3DPreview)) ||
			FAILED(m_pDevice->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &m_pPS_Depth3DPreview)) ||
			FAILED(m_pDevice->CreateBuffer(&constants, nullptr, &m_pDepth3DPreviewConstants))) {
			m_depth3DPreviewShaderFailed = true;
			return false;
		}
		D3D11_INPUT_ELEMENT_DESC layout[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		};
		if (FAILED(m_pDevice->CreateInputLayout(layout, std::size(layout), vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &m_pVDepth3DPreviewInputLayout))) {
			m_depth3DPreviewShaderFailed = true;
			return false;
		}
	}

	if (!m_pDepth3DPreviewVertexBuffer || !m_pDepth3DPreviewIndexBuffer || !m_depth3DPreviewIndexCount) {
		// Independent depth tiles instead of one continuous cloth-like mesh.
		// Each tile uses one representative depth for all four of its corners,
		// so hard depth changes become height steps rather than stretched slopes.
		constexpr UINT gridX = 192;
		constexpr UINT gridY = 108;
		constexpr float overlap = 1.03f; // small XY overlap to hide cracks between tiles
		std::vector<DEPTH3D_VERTEX> vertices;
		vertices.reserve(gridX * gridY * 4);
		std::vector<uint32_t> indices;
		indices.reserve(gridX * gridY * 6);
		for (UINT y = 0; y < gridY; y++) {
			const float v0 = float(y) / gridY;
			const float v1 = float(y + 1) / gridY;
			const float vc = (v0 + v1) * 0.5f;
			for (UINT x = 0; x < gridX; x++) {
				const float u0 = float(x) / gridX;
				const float u1 = float(x + 1) / gridX;
				const float uc = (u0 + u1) * 0.5f;
				const float halfW = (u1 - u0) * 0.5f * overlap;
				const float halfH = (v1 - v0) * 0.5f * overlap;
				const float px0 = uc - halfW;
				const float px1 = uc + halfW;
				const float py0 = vc - halfH;
				const float py1 = vc + halfH;
				const uint32_t base = uint32_t(vertices.size());
				const DirectX::XMFLOAT2 dc{uc, vc};
				vertices.push_back({{px0, py0}, {u0, v0}, dc});
				vertices.push_back({{px1, py0}, {u1, v0}, dc});
				vertices.push_back({{px0, py1}, {u0, v1}, dc});
				vertices.push_back({{px1, py1}, {u1, v1}, dc});
				indices.push_back(base + 0);
				indices.push_back(base + 2);
				indices.push_back(base + 1);
				indices.push_back(base + 1);
				indices.push_back(base + 2);
				indices.push_back(base + 3);
			}
		}
		D3D11_BUFFER_DESC vb{};
		vb.ByteWidth = UINT(vertices.size() * sizeof(DEPTH3D_VERTEX));
		vb.Usage = D3D11_USAGE_IMMUTABLE;
		vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA vd{vertices.data()};
		D3D11_BUFFER_DESC ib{};
		ib.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
		ib.Usage = D3D11_USAGE_IMMUTABLE;
		ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA id{indices.data()};
		if (FAILED(m_pDevice->CreateBuffer(&vb, &vd, &m_pDepth3DPreviewVertexBuffer)) ||
			FAILED(m_pDevice->CreateBuffer(&ib, &id, &m_pDepth3DPreviewIndexBuffer))) {
			m_depth3DPreviewShaderFailed = true;
			return false;
		}
		m_depth3DPreviewIndexCount = UINT(indices.size());
	}

	if (!m_pDepth3DPreviewRasterizer) {
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.FrontCounterClockwise = FALSE;
		desc.DepthClipEnable = TRUE;
		desc.ScissorEnable = FALSE;
		desc.MultisampleEnable = FALSE;
		desc.AntialiasedLineEnable = FALSE;
		if (FAILED(m_pDevice->CreateRasterizerState(&desc, &m_pDepth3DPreviewRasterizer))) {
			m_depth3DPreviewShaderFailed = true;
			return false;
		}
	}

	return true;
}

const Tex2D_t* CDX11VideoProcessor::PrepareDepth3DPreview(const CompletedFrame& frame, const CRect& sourceRect)
{
	if (!m_depth3DPreview || m_depth3DPreviewShaderFailed || sourceRect.IsRectEmpty() ||
		(!UsesZipDepthSr() && !UsesZipDepthMfg())) return &frame.image;

	m_zipDepth.PollCompleted();
	const auto estimate = m_zipDepth.ForFrame(frame.pts);
	if (!estimate.view) return &frame.image;
	if (!EnsureDepth3DMeshResources()) return &frame.image;

	if (FAILED(m_depth3DPreviewTexture.CheckCreate(m_pDevice, frame.image.desc.Format,
		frame.image.desc.Width, frame.image.desc.Height, Tex2D_DefaultShaderRTarget))) return &frame.image;

	CComPtr<ID3D11RenderTargetView> targetView;
	if (FAILED(m_pDevice->CreateRenderTargetView(m_depth3DPreviewTexture.pTexture, nullptr, &targetView))) return &frame.image;

	const float sourceUV[4]{float(sourceRect.left) / frame.image.desc.Width,
		float(sourceRect.top) / frame.image.desc.Height,
		float(sourceRect.Width()) / frame.image.desc.Width,
		float(sourceRect.Height()) / frame.image.desc.Height};
	// Independent tile settings. Each tile keeps one depth height across the whole
	// quad, then the vertex shader rotates the stepped surface 30 degrees around Y
	// and applies a small perspective projection.
	constexpr float kDepth3DZStrength = 0.45f;
	constexpr float kDepth3DCameraDistance = 3.0f;
	const float constants[8]{sourceUV[0], sourceUV[1], sourceUV[2], sourceUV[3],
		estimate.rotated ? 1.0f : 0.0f, kDepth3DZStrength, CZipDepth::NormalizedDepthSpan, kDepth3DCameraDistance};
	m_pDeviceContext->UpdateSubresource(m_pDepth3DPreviewConstants, 0, nullptr, constants, 0, 0);

	const float black[4]{0, 0, 0, 1};
	m_pDeviceContext->ClearRenderTargetView(targetView, black);
	D3D11_VIEWPORT viewport{FLOAT(sourceRect.left), FLOAT(sourceRect.top), FLOAT(sourceRect.Width()),
		FLOAT(sourceRect.Height()), 0.0f, 1.0f};

	const UINT stride = sizeof(DEPTH3D_VERTEX), offset = 0;
	ID3D11ShaderResourceView* vsDepth = estimate.view;
	ID3D11ShaderResourceView* psImage = frame.image.pShaderResource;
	CComPtr<ID3D11RasterizerState> previousRasterizer;
	m_pDeviceContext->RSGetState(&previousRasterizer);
	m_pDeviceContext->RSSetState(m_pDepth3DPreviewRasterizer);
	m_pDeviceContext->IASetInputLayout(m_pVDepth3DPreviewInputLayout);
	m_pDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_pDeviceContext->IASetVertexBuffers(0, 1, &m_pDepth3DPreviewVertexBuffer.p, &stride, &offset);
	m_pDeviceContext->IASetIndexBuffer(m_pDepth3DPreviewIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
	m_pDeviceContext->OMSetRenderTargets(1, &targetView.p, nullptr);
	m_pDeviceContext->RSSetViewports(1, &viewport);
	m_pDeviceContext->VSSetShader(m_pVS_Depth3DPreview, nullptr, 0);
	m_pDeviceContext->PSSetShader(m_pPS_Depth3DPreview, nullptr, 0);
	m_pDeviceContext->VSSetShaderResources(0, 1, &vsDepth);
	m_pDeviceContext->PSSetShaderResources(1, 1, &psImage);
	m_pDeviceContext->VSSetSamplers(0, 1, &m_pSamplerPoint.p);
	m_pDeviceContext->PSSetSamplers(0, 1, &m_pSamplerLinear.p);
	m_pDeviceContext->VSSetConstantBuffers(0, 1, &m_pDepth3DPreviewConstants.p);
	m_pDeviceContext->PSSetConstantBuffers(0, 1, &m_pDepth3DPreviewConstants.p);
	m_pDeviceContext->DrawIndexed(m_depth3DPreviewIndexCount, 0, 0);
	m_pDeviceContext->RSSetState(previousRasterizer);

	ID3D11ShaderResourceView* emptyViews[2]{};
	m_pDeviceContext->VSSetShaderResources(0, 1, emptyViews);
	m_pDeviceContext->PSSetShaderResources(1, 1, emptyViews);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	return &m_depth3DPreviewTexture;
}

void CDX11VideoProcessor::DrawDepthOverlay(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp)
{
	m_zipDepth.PollCompleted();
	const auto estimate = m_zipDepth.ForFrame(timestamp);
	if (!estimate.view) return;
	const auto tensor = m_zipDepth.TensorSize();
	DrawDepthView(target, picture, estimate.view, tensor, CRect(0, 0, tensor.cx, tensor.cy),
		estimate.rotated ? 270 : 0, false);
}

void CDX11VideoProcessor::DrawDepthView(ID3D11Texture2D* target, const CRect& picture,
	ID3D11ShaderResourceView* depth, SIZE textureSize, const CRect& source, int rotation, bool inverted)
{
	if (textureSize.cx <= 0 || textureSize.cy <= 0 || picture.IsRectEmpty()) return;
	if (!m_pPS_DepthOverlay) {
		static constexpr char source[] = R"(
Texture2D<float> Depth : register(t0);
SamplerState Linear : register(s0);
cbuffer Settings : register(b0) { float Inverted; float DepthSpan; };
// Match the Simple VS signature so TEXCOORD stays in the same register.
struct PS_INPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
float4 main(PS_INPUT input) : SV_TARGET {
    const float sampleDepth = Depth.SampleLevel(Linear, input.Tex, 0);
    const float depth = Inverted > 0.5 ? 1.0 - sampleDepth : sampleDepth;
    const float gray = saturate(0.5 + (0.5 - depth) / DepthSpan);
    return float4(gray, gray, gray, 1.0);
}
)";
		D3D11_BLEND_DESC blend{};
		auto& color = blend.RenderTarget[0];
		color.BlendEnable = TRUE;
		color.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
		color.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
		color.BlendOp = D3D11_BLEND_OP_ADD;
		color.SrcBlendAlpha = D3D11_BLEND_ONE;
		color.DestBlendAlpha = D3D11_BLEND_ZERO;
		color.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		color.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
			D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		CComPtr<ID3DBlob> code;
		const D3D11_BUFFER_DESC constants{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
		if (FAILED(CompileShader(source, nullptr, "ps_4_0", &code)) ||
			FAILED(m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
				nullptr, &m_pPS_DepthOverlay)) ||
			FAILED(m_pDevice->CreateBlendState(&blend, &m_pDepthOverlayBlendState)) ||
			FAILED(m_pDevice->CreateBuffer(&constants, nullptr, &m_pDepthOverlayConstants))) {
			m_depthOverlayShaderFailed = true;
			return;
		}
	}
	CComPtr<ID3D11RenderTargetView> targetView;
	if (FAILED(m_pDevice->CreateRenderTargetView(target, nullptr, &targetView)) ||
		FAILED(FillVertexBuffer(m_pDeviceContext, m_pVertexBuffer, textureSize.cx, textureSize.cy, source, rotation, false))) return;
	const float settings[4]{inverted ? 1.f : 0.f,
		inverted ? CZipDepthHistory::OutputDepthSpan : CZipDepth::NormalizedDepthSpan};
	m_pDeviceContext->UpdateSubresource(m_pDepthOverlayConstants, 0, nullptr, settings, 0, 0);
	D3D11_VIEWPORT viewport{FLOAT(picture.left), FLOAT(picture.top),
		FLOAT(picture.Width()), FLOAT(picture.Height()), 0.0f, 1.0f};
	m_pDeviceContext->RSSetState(nullptr);
	constexpr float opacity = 0.95f;
	const FLOAT blendFactor[]{opacity, opacity, opacity, 1.0f};
	TextureBlt11(m_pDeviceContext, targetView, viewport, m_pVSimpleInputLayout, m_pVS_Simple,
		m_pPS_DepthOverlay, depth, m_pSamplerLinear, m_pDepthOverlayConstants, m_pVertexBuffer,
		m_pDepthOverlayBlendState, blendFactor);
	m_pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
}

void CDX11VideoProcessor::DrawSubtitles(ID3D11Texture2D* pRenderTarget, const CRect* picture)
{
	HRESULT hr = S_OK;
	const CRect& subtitleRect = picture ? *picture : m_videoRect;

	CComPtr<ISubPic> pSubPic = m_pFilter->GetSubPic(m_rtStart);
	if (pSubPic) {
		RECT rcSource, rcDest;
		hr = pSubPic->GetSourceAndDest(m_windowRect, subtitleRect, &rcSource, &rcDest, FALSE, {}, 0, FALSE);
		if (SUCCEEDED(hr)) {
			ID3D11RenderTargetView* pRenderTargetView;
			hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
			if (SUCCEEDED(hr)) {
				// Set render target and shaders
				m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
				m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
				m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
				m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);
				if (m_bPGSSubtitles && SourceIsHDR10() && m_bConvertToSdr && m_pCorrectionConstants) {
					m_pDeviceContext->PSSetConstantBuffers(0, 1, &m_pCorrectionConstants.p);
				}

				// call the function for drawing subtitles
				hr = pSubPic->AlphaBlt(&rcSource, &rcDest, nullptr);

				pRenderTargetView->Release();
			}
		}
		return;
	}

	if (m_pFilter->m_pSub11CallBack) {
		ID3D11RenderTargetView* pRenderTargetView;
		hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
		if (SUCCEEDED(hr)) {
			const CRect rSrcPri(POINT(0, 0), m_windowRect.Size());
			const CRect rDstVid(subtitleRect);
			const auto rtStart = m_pFilter->m_rtStartTime + m_rtStart;

			// Set render target and shaders
			m_pDeviceContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
			m_pDeviceContext->IASetInputLayout(m_pVSimpleInputLayout);
			m_pDeviceContext->VSSetShader(m_pVS_Simple, nullptr, 0);
			m_pDeviceContext->PSSetShader(m_pPS_BitmapToFrame, nullptr, 0);
			if (m_bPGSSubtitles && SourceIsHDR10() && m_bConvertToSdr && m_pCorrectionConstants) {
				m_pDeviceContext->PSSetConstantBuffers(0, 1, &m_pCorrectionConstants.p);
			}

			// call the function for drawing subtitles
			hr = m_pFilter->m_pSub11CallBack->Render11(rtStart, 0, m_rtAvgTimePerFrame, rDstVid, rDstVid, rSrcPri,
													   1., m_iStereo3dTransform == 1 ? m_nStereoSubtitlesOffsetInPixels : 0);

			pRenderTargetView->Release();
		}
	}
}

bool CDX11VideoProcessor::DlssSRSourceAllowed(const CRect* preparedRect) const
{
    // Use exactly the same decoded-source size thresholds as RTX VSR.
    const UINT width = preparedRect ? preparedRect->Width() : m_srcWidth;
    const UINT height = preparedRect ? preparedRect->Height() : m_srcHeight;
    if (!width || !height) return false;
    const UINT longSide = std::max(width, height);
    const UINT shortSide = std::min(width, height);
    switch (m_dlssSRSourceLimit) {
    case DLSSSR_SOURCE_SD:    return longSide <= 1024 && shortSide <= 576;
    case DLSSSR_SOURCE_720p:  return longSide <= 1280 && shortSide <= 720;
    case DLSSSR_SOURCE_1080p: return longSide <= 2048 && shortSide <= 1088;
    case DLSSSR_SOURCE_1440p: return longSide <= 2560 && shortSide <= 1440;
    default: return false;
    }
}

bool CDX11VideoProcessor::CanDlssSRUpscale(const CRect& srcRect, const CRect& dstRect, bool preparedRgb)
{
	return m_iVPSuperRes == SUPERRES_DLSS_SR && DlssSRSourceAllowed(preparedRgb ? &srcRect : nullptr)
		&& m_VendorId == PCIV_NVIDIA && (preparedRgb || (m_D3D11VP.IsReady() && m_srcParams.CDepth == 8)) &&
		(m_SampleFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE || m_iVPDeinterlacing != DEINT_Disable) &&
		!SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_iRotation && !m_bFlip && (!m_resizePending || m_processingSize.cx > 0) &&
		!srcRect.IsRectEmpty() &&
		((dstRect.Width() > srcRect.Width() && dstRect.Height() > srcRect.Height()) ||
		 (m_dlssSRDlaa && dstRect.Width() == srcRect.Width() && dstRect.Height() == srcRect.Height()));
}

Tex2D_t* CDX11VideoProcessor::PrepareDlssSR(const CRect& srcRect, const CRect& dstRect, bool afterMfg)
{
	if (m_dlssSRAfterMfg != afterMfg) {
		m_dlssSR.ResetHistory();
		m_dlssSRAfterMfg = afterMfg;
	}
	m_dlssSRInputStatus = nullptr;
	if (UsesZipDepthSr()) m_zipDepth.Prepare(m_pDevice);
	return m_dlssSR.Prepare(m_pDevice, srcRect.Width(), srcRect.Height(),
		dstRect.Width(), dstRect.Height(), !m_pFilter->m_Sets.bWaitForExtendedStartup);
}

Tex2D_t* CDX11VideoProcessor::EvaluateDlssSR(const Tex2D_t& source, Tex2D_t& input, const CRect& sourceRect,
                                             REFERENCE_TIME timestamp, float frameTimeMs, bool* overlayInputHandled, bool depthPrepared)
{
	if (FAILED(TextureCopyRect(source, input.pTexture, sourceRect, sourceRect, m_pPS_Simple, nullptr, 0, false))) {
		m_dlssSRInputStatus = L"source conversion failed";
		m_dlssSR.ResetHistory();
		return nullptr;
	}
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11ShaderResourceView* empty = nullptr;
	m_pDeviceContext->PSSetShaderResources(0, 1, &empty);
	const bool sourceFrame = !m_dlssSRAfterMfg;
	const bool overlay = sourceFrame && overlayInputHandled && !m_mfgMultiplier &&
		!depthPrepared && BlendSourceDepthOverlay(input.pTexture, source.pShaderResource, sourceRect, timestamp);
	ID3D11ShaderResourceView* cleanSource = depthPrepared || overlay ? source.pShaderResource.p : nullptr;
	auto* output = m_dlssSR.Evaluate(timestamp, frameTimeMs, UsesZipDepthSr() ? &m_zipDepth : nullptr,
		sourceFrame, cleanSource);
	if (overlayInputHandled) *overlayInputHandled = output && overlay;
	return output;
}

Tex2D_t* CDX11VideoProcessor::TryDlssSR(const CRect& srcRect, const CRect& dstRect, const bool second, const REFERENCE_TIME timestamp, bool* overlayInputHandled, const Tex2D_t* preparedSource)
{
	if (m_iVPSuperRes != SUPERRES_DLSS_SR) {
		return nullptr;
	}
	if (!CanDlssSRUpscale(srcRect, dstRect, preparedSource != nullptr)) {
        m_dlssSRInputStatus = !DlssSRSourceAllowed(preparedSource ? &srcRect : nullptr) ? L"source exceeds DLSS size threshold" :
            m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE && m_iVPDeinterlacing == DEINT_Disable
            ? L"enable D3D11 deinterlacing for this source"
            : preparedSource ? L"requires NVIDIA and an SDR upscale (or native DLAA)"
            : L"requires NVIDIA D3D11 VP and an 8-bit SDR upscale (or native DLAA)";
		return nullptr;
	}
	const CRect nativeRect(0, 0, srcRect.Width(), srcRect.Height());
	auto* input = PrepareDlssSR(nativeRect, dstRect, false);
	if (!input) {
		return nullptr;
	}
	const float frameTimeMs = m_rtAvgTimePerFrame > 0 ? float(m_rtAvgTimePerFrame / 10000.0) : 41.7f;
	if (preparedSource) {
		return EvaluateDlssSR(*preparedSource, *input, nativeRect, timestamp, frameTimeMs, overlayInputHandled, true);
	}
	if (FAILED(m_dlssSRSource.CheckCreate(m_pDevice, m_D3D11OutputFmt, srcRect.Width(), srcRect.Height(), Tex2D_DefaultShaderRTarget)) ||
	    FAILED(D3D11VPPass(m_dlssSRSource.pTexture, srcRect, nativeRect, second))) {
		m_dlssSRInputStatus = L"source conversion failed";
		return nullptr;
	}
	return EvaluateDlssSR(m_dlssSRSource, *input, nativeRect, timestamp, m_bDoubleFrames ? frameTimeMs / 2 : frameTimeMs, overlayInputHandled);
}

bool CDX11VideoProcessor::CanProjectJvr() const
{
	// The manual JVR preset describes progressive SDR full-SBS, two square eyes.
	return m_jvrFlatView && !m_jvrShaderFailed && !m_srcRect.IsRectEmpty() && m_srcRect.Height() >= 2 &&
		int64_t(m_srcRect.Width()) == 2LL * m_srcRect.Height() &&
		!m_bInterlaced && m_SampleFormat == D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE &&
		!SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_iRotation && !m_bFlip &&
		!m_pPSHalfOUtoInterlace;
}

void CDX11VideoProcessor::ConfigureJvrProjection(const Settings_t& config)
{
	m_jvrEye = config.iJvrEye == 1 ? 1 : 0;
	const int vertical = std::clamp(config.iJvrVertical, JVR_VERTICAL_MIN, JVR_VERTICAL_MAX);
	const int reprojection = std::clamp(config.iJvrReprojection, JVR_REPROJECTION_MIN, JVR_REPROJECTION_MAX);
	m_jvrView = jvr::MakeView(vertical, reprojection);
}

CRect CDX11VideoProcessor::ProcessingSourceRect(bool projected) const
{
	if (!projected) return m_srcRect;
	const auto size = jvr::SourceFrameSize(m_srcRect.Height());
	return CRect(0, 0, size.width, size.height);
}

CRect CDX11VideoProcessor::PictureForPlayback(bool projected) const
{
	if (!projected) return m_videoRect;
	const auto size = jvr::FitFrame(m_windowRect.Width(), m_windowRect.Height());
	const LONG left = m_windowRect.left + (m_windowRect.Width() - size.width) / 2;
	const LONG top = m_windowRect.top + (m_windowRect.Height() - size.height) / 2;
	return CRect(left, top, left + size.width, top + size.height);
}

HRESULT CDX11VideoProcessor::PrepareJvrFrame(const CRect& picture, bool second)
{
	if (m_jvrShaderFailed) return E_FAIL;
	const Tex2D_t* source = &m_TexSrcVideo;
	CRect sourceRect = m_srcRect;
	if (m_D3D11VP.IsReady()) {
		auto hr = m_jvrSource.CheckCreate(m_pDevice, m_D3D11OutputFmt, sourceRect.Width(), sourceRect.Height(),
			Tex2D_DefaultShaderRTarget);
		if (FAILED(hr)) return hr;
		const CRect native(0, 0, sourceRect.Width(), sourceRect.Height());
		hr = D3D11VPPass(m_jvrSource.pTexture, sourceRect, native, second);
		if (FAILED(hr)) return hr;
		source = &m_jvrSource;
		sourceRect = native;
	} else if (m_PSConvColorData.bEnable) {
		const auto hr = ConvertColorPass(m_TexConvertOutput.pTexture);
		if (FAILED(hr)) return hr;
		source = &m_TexConvertOutput;
		sourceRect.SetRect(0, 0, source->desc.Width, source->desc.Height);
	}
	const auto hr = m_jvrFrame.CheckCreate(m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM,
		picture.Width(), picture.Height(), Tex2D_DefaultShaderRTarget);
	if (FAILED(hr)) return hr;
	const auto result = ProjectJvr(*source, m_jvrFrame.pTexture, sourceRect, picture);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	return result;
}

HRESULT CDX11VideoProcessor::ProjectJvr(const Tex2D_t& source, ID3D11Texture2D* target,
	const CRect& sourceRect, const CRect& picture)
{
	if (m_jvrShaderFailed || !source.pShaderResource || sourceRect.IsRectEmpty() || picture.IsRectEmpty()) {
		return E_FAIL;
	}
	using Constants = std::array<float, 16>;
	static_assert(sizeof(Constants) == 4 * 16);
	if (!m_jvrShader) {
		static constexpr char shader[] = R"(
Texture2D<float4> Video : register(t0);
SamplerState Linear : register(s0);
cbuffer Projection : register(b0) {
    float4 Viewport; // centre in output pixels and plane units per pixel
    float4 Eye;      // selected eye's texture UV origin and extent
    float4 Lens;     // half a source texel and the eye's local optical centre
    float4 View;     // focal length, cosine and sine of pitch, vertical plane offset
};

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 plane = (position.xy - Viewport.xy) * Viewport.zw;
    plane.y += View.w;

    // Inverse stereographic projection. The common normalization factor
    // can be omitted because the source lens uses the ray's angle.
    float3 ray = float3(plane, 1.0 - 0.25 * dot(plane, plane));
    const float y = View.y * ray.y + View.z * ray.z;
    const float z = View.y * ray.z - View.z * ray.y;
    const float2 radial = float2(ray.x, y);
    const float radius = length(radial);
    // Extreme pitch can include the rear optical axis. Its lens azimuth is undefined.
    if (z < 0.0 && radius < 1e-7) return float4(0, 0, 0, 1);
    const float angle = atan2(radius, z);
    const float2 eyeUV = Lens.zw + radial * (View.x * angle / max(radius, 1e-7));
    if (any(eyeUV < 0.0) || any(eyeUV > 1.0)) return float4(0, 0, 0, 1);

    // Keep bilinear filtering inside the selected eye even at the SBS seam.
    const float2 uv = clamp(Eye.xy + Eye.zw * eyeUV, Eye.xy + Lens.xy,
        Eye.xy + Eye.zw - Lens.xy);
    return Video.SampleLevel(Linear, uv, 0);
}
)";
		CComPtr<ID3DBlob> bytecode;
		HRESULT hr = CompileShader(shader, nullptr, "ps_4_0", &bytecode);
		if (SUCCEEDED(hr)) {
			hr = m_pDevice->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &m_jvrShader);
		}
		if (SUCCEEDED(hr)) {
			const D3D11_BUFFER_DESC desc{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
			hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_jvrConstants);
		}
		if (FAILED(hr)) {
			m_jvrShader.Release();
			m_jvrShaderFailed = true;
			DLog(L"JVR flat view: shader initialization failed ({})", HR2Str(hr));
			return hr;
		}
		m_jvrUploadedConstants = {};
	}

	constexpr jvr::Profile lens{};
	const auto& eye = lens.eyes[m_jvrEye];
	const float invW = 1.0f / source.desc.Width;
	const float invH = 1.0f / source.desc.Height;
	const float eyeWidth = 0.5f * sourceRect.Width();
	const Constants constants{
		picture.left + picture.Width() * 0.5f, picture.top + picture.Height() * 0.5f,
		float(2 * jvr::PlaneHalfWidth / picture.Width()), float(2 * jvr::PlaneHalfHeight / picture.Height()),
		(sourceRect.left + m_jvrEye * eyeWidth) * invW, sourceRect.top * invH, eyeWidth * invW, sourceRect.Height() * invH,
		0.5f * invW, 0.5f * invH, eye.x, eye.y,
		eye.focal, m_jvrView.cosPitch, m_jvrView.sinPitch, m_jvrView.verticalShift
	};
	if (constants != m_jvrUploadedConstants) {
		m_pDeviceContext->UpdateSubresource(m_jvrConstants, 0, nullptr, constants.data(), 0, 0);
		m_jvrUploadedConstants = constants;
	}
	return TextureCopyRect(source, target, sourceRect, picture, m_jvrShader, m_jvrConstants, 0, false, true);
}

HRESULT CDX11VideoProcessor::Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, const bool second, const bool allowDlssSR, const REFERENCE_TIME timestamp, CTex2DRing* processingPostScale, bool* overlayInputHandled, const Tex2D_t* preparedSource)
{
	if (overlayInputHandled) *overlayInputHandled = false;
	HRESULT hr = S_OK;
	m_bDitherUsed = false;
	int rotation = m_iRotation;

	CRect rSrc = srcRect;
	const Tex2D_t* pInputTexture = nullptr;

	const UINT numSteps = GetPostScaleSteps();
	auto& postScale = processingPostScale ? *processingPostScale : m_TexsPostScale;

	const bool srWasActive = m_dlssSR.Active();
	auto* srOutput = allowDlssSR ? TryDlssSR(srcRect, dstRect, second, timestamp, overlayInputHandled, preparedSource) : nullptr;
	if (allowDlssSR && !srOutput && srWasActive) {
		m_dlssSR.ResetHistory();
	}
	if (allowDlssSR && srWasActive != bool(srOutput) && m_mfgMultiplier && !MfgSourceMeetsDisplayRate()) {
		if (m_pFilter->m_Sets.bWaitForExtendedStartup) {
			StopMfg(false);
		} else {
			// Completed originals still belong to the previous visual route.
			// Keep their deadlines and restart interpolation at this boundary.
			ResetMfgFrameAnalysis();
			m_mfgRepeatTiming.ClearEvidence();
		}
		m_mfg.ResetHistory();
	}
	if (srOutput) {
		pInputTexture = srOutput;
		rSrc.SetRect(0, 0, srOutput->desc.Width, srOutput->desc.Height);
	}
	else if (preparedSource) {
		pInputTexture = preparedSource;
		m_bVPScalingUseShaders = m_D3D11VP.IsReady();
	}
	else if (m_D3D11VP.IsReady()) {
		if (!(m_iSwapEffect == SWAPEFFECT_Discard && (m_VendorId == PCIV_AMDATI || m_VendorId == PCIV_INTEL))) {
			const bool bNeedShaderTransform =
				(m_TexConvertOutput.desc.Width != dstRect.Width() || m_TexConvertOutput.desc.Height != dstRect.Height() || m_bFlip
				|| (!processingPostScale && (dstRect.right > m_windowRect.right || dstRect.bottom > m_windowRect.bottom)))
				|| (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)); // At least on Nvidia we can sometimes get the "D3D11: Removing Device" error here when HDR Passthrough.
			if (!bNeedShaderTransform && !numSteps) {
				m_bVPScalingUseShaders = false;
				hr = D3D11VPPass(pRenderTarget, rSrc, dstRect, second);

				return hr;
			}
		}

		CRect rect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
		hr = D3D11VPPass(m_TexConvertOutput.pTexture, rSrc, rect, second);
		if (FAILED(hr)) return hr;
		pInputTexture = &m_TexConvertOutput;
		rSrc = rect;
		rotation = 0;
	}
	else if (m_PSConvColorData.bEnable) {
		hr = ConvertColorPass(m_TexConvertOutput.pTexture);
		if (FAILED(hr)) return hr;
		pInputTexture = &m_TexConvertOutput;
		rSrc.SetRect(0, 0, m_TexConvertOutput.desc.Width, m_TexConvertOutput.desc.Height);
	}
	else {
		pInputTexture = &m_TexSrcVideo;
	}

	if (numSteps) {
		UINT step = 0;
		const Tex2D_t* pTex = postScale.GetFirstTex();
		ID3D11Texture2D* pRT = pTex->pTexture;

		auto StepSetting = [&]() {
			step++;
			pInputTexture = pTex;
			if (step < numSteps) {
				pTex = postScale.GetNextTex();
				pRT = pTex->pTexture;
			} else {
				pRT = pRenderTarget;
			}
		};

		CRect rect;
		rect.IntersectRect(dstRect, CRect(0, 0, pTex->desc.Width, pTex->desc.Height));

		if (m_D3D11VP.IsReady()) {
			m_bVPScalingUseShaders = preparedSource || rSrc.Width() != dstRect.Width() || rSrc.Height() != dstRect.Height();
		}

		if (preparedSource || srOutput || rSrc != dstRect || rotation != 0) {
			hr = ResizeShaderPass(*pInputTexture, pRT, rSrc, dstRect, rotation);
			if (preparedSource && FAILED(hr)) return hr;
		} else {
			pTex = pInputTexture; // Hmm
		}

		if (m_pPSCorrection) {
			StepSetting();
			const bool bMeasured = m_bSdrMeasureActive && MeasureHdrPeak(*pInputTexture, rect) == S_OK;
			if (bMeasured) {
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_pHdrStateSRV.p);
			}
			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSCorrection, m_pCorrectionConstants, 0, false);
			if (bMeasured) {
				ID3D11ShaderResourceView* pNullSRV = nullptr;
				m_pDeviceContext->PSSetShaderResources(1, 1, &pNullSRV);
				ReadHdrMeasureStats();
			}
		}

		if (m_pPSHDR10ToneMapping) {
			StepSetting();

			if (m_pDoViDynamicConstants) {
				m_pDeviceContext->PSSetConstantBuffers(1, 1, &m_pDoViDynamicConstants.p);
			}

			const bool bMeasured = m_bHdrLocalMeasureActive && MeasureHdrPeak(*pInputTexture, rect) == S_OK;
			if (bMeasured) {
				m_pDeviceContext->PSSetShaderResources(1, 1, &m_pHdrStateSRV.p);
			}
			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSHDR10ToneMapping, m_pHDR10ToneMappingConstants, 0, false);
			if (bMeasured) {
				ID3D11ShaderResourceView* pNullSRV = nullptr;
				m_pDeviceContext->PSSetShaderResources(1, 1, &pNullSRV);
				ReadHdrMeasureStats();
			}
		}

		if (m_pPostScaleShaders.size()) {
			static __int64 counter = 0;
			static long start = GetTickCount();

			long stop = GetTickCount();
			long diff = stop - start;
			if (diff >= 10 * 60 * 1000) {
				start = stop;    // reset after 10 min (ps float has its limits in both range and accuracy)
			}

			PS_EXTSHADER_CONSTANTS ConstData = {
				{1.0f / pTex->desc.Width, 1.0f / pTex->desc.Height },
				{(float)pTex->desc.Width, (float)pTex->desc.Height},
				counter++,
				(float)diff / 1000,
				0, 0
			};
			m_pDeviceContext->UpdateSubresource(m_pPostScaleConstants, 0, nullptr, &ConstData, 0, 0);

			for (UINT idx = 0; idx < m_pPostScaleShaders.size(); idx++) {
				StepSetting();
				hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPostScaleShaders[idx].shader, m_pPostScaleConstants, 0, false);
			}
		}

		if (m_pPSHalfOUtoInterlace) {
			DrawSubtitles(pRT);

			StepSetting();
			FLOAT ConstData[] = {
				(float)pTex->desc.Height, 0,
				(float)dstRect.top / pTex->desc.Height, (float)dstRect.bottom / pTex->desc.Height,
			};
			D3D11_MAPPED_SUBRESOURCE mr;
			hr = m_pDeviceContext->Map(m_pHalfOUtoInterlaceConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
			if (SUCCEEDED(hr)) {
				memcpy(mr.pData, &ConstData, sizeof(ConstData));
				m_pDeviceContext->Unmap(m_pHalfOUtoInterlaceConstantBuffer, 0);
			}
			hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSHalfOUtoInterlace, m_pHalfOUtoInterlaceConstantBuffer, 0, false);
		}

		if (m_bFinalPass) {
			StepSetting();
			hr = FinalPass(*pTex, pRT, rect, rect);
			m_bDitherUsed = true;
		}
	}
	else {
		hr = ResizeShaderPass(*pInputTexture, pRenderTarget, rSrc, dstRect, rotation);
	}

	DLogIf(FAILED(hr), L"CDX11VideoProcessor::Process() : failed with error {}", HR2Str(hr));

	return hr;
}

void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	if (m_videoRect != videoRect) m_completedGeometryTick = GetTickCount64();
	m_videoRect = videoRect;
	UpdateRenderRect();
	// Compute the display-sized offscreen target before allocating the VP
	// conversion texture. A window-size event must not resize VSR output.
	UpdateProcessingResolution();
	UpdateTexures();
}

// Work on a video-sized texture independently from the swap chain.
CRect CDX11VideoProcessor::ProcessingRect() const
{
	return m_processingSize.cx > 0 ? CRect(CPoint(0, 0), m_processingSize) : PictureForPlayback(CanProjectJvr());
}

void CDX11VideoProcessor::UpdateProcessingResolution()
{
	const auto previous = m_processingSize;
	if ((m_iVPSuperRes == SUPERRES_Disable && (!m_mfgMultiplier || MfgSourceMeetsDisplayRate())) || m_pPSHalfOUtoInterlace) {
		m_processingSize = CSize(0, 0);
		if (previous.cx > 0) {
			m_processingTexture.Release();
			m_processingPostScale.Release();
			if (m_D3D11VP.IsReady() && m_bVPScaling) UpdateTexures();
		}
		return;
	}
	if (m_videoRect.IsRectEmpty() || m_srcRectWidth <= 0 || m_srcRectHeight <= 0) {
		return;
	}

	const auto& display = m_pFilter->m_DisplayConfig;
	const bool projected = CanProjectJvr();
	const double ratio = projected ? double(jvr::AspectWidth) / jvr::AspectHeight : m_srcAspectRatioX && m_srcAspectRatioY
		? double(m_srcAspectRatioX) / m_srcAspectRatioY : double(m_srcRectWidth) / m_srcRectHeight;
	const bool rotated = m_iRotation % 180 != 0;
	const double aspect = rotated ? 1.0 / ratio : ratio;
	if (display.width && display.height) {
		// Fit the display without adding letterbox bars to the inference input.
		const auto width = std::min<double>(display.width, display.height * aspect);
		m_processingSize = CSize(std::max(1L, LONG(std::lround(width))),
			std::max(1L, LONG(std::lround(width / aspect))));
	} else {
		// Keep a window-independent canvas even without display mode information.
		const LONG height = projected ? ProcessingSourceRect(true).Height() : rotated ? m_srcRectWidth : m_srcRectHeight;
		m_processingSize = CSize(std::max(1L, LONG(std::lround(height * aspect))), height);
	}
	if (previous.cx > 0 && previous != m_processingSize && m_mfgMultiplier) {
		StopMfg(false);
		m_mfg.ResetHistory();
		m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
		m_mfgInputModeKnown = false;
	}
	// Display mode/source changes can arrive without a SetVideoRect event.
	// Keep the VP intermediate at the same fixed size before the next frame.
	if (previous != m_processingSize && m_D3D11VP.IsReady() && m_bVPScaling) {
		UpdateTexures();
	}
}

HRESULT CDX11VideoProcessor::SetWindowRect(const CRect& windowRect)
{
    if (m_windowRect != windowRect || m_resizePending) m_completedGeometryTick = GetTickCount64();
    const bool sizeChanged = windowRect.Width() != m_windowRect.Width() ||
                             windowRect.Height() != m_windowRect.Height();
    if (!sizeChanged) {
        // Position changes are not swap-chain resizes. When a drag returns to
        // the original size, discard the uncommitted resize request. Treat
        // cancellation of a pending size as the final geometry event so the
        // completed-frame pacing estimator also gets a clean settling window.
        if (m_resizePending) m_resizeLastChangeTick = GetTickCount64();
        m_resizePending = false;
        m_windowRect = windowRect;
        UpdateRenderRect();
        return S_OK;
    }

    // During a continuous drag, resizing DXGI buffers and tearing down NGX on
    // every WM_SIZE event is extremely expensive. Keep rendering the source to
    // the last committed buffer until the final dimensions settle. This path
    // only applies to windowed playback with MFG enabled and a running graph.
    const bool defer = m_pDXGISwapChain1 && !m_bExclusiveScreen &&
        (m_mfgMultiplier.load(std::memory_order_relaxed) != 0 ||
         CompletedFramesPending() || m_completedLast) &&
        m_pFilter->m_filterState == State_Running &&
        windowRect.Width() > 0 && windowRect.Height() > 0;
    if (defer) {
        if (!m_resizePending && m_processingSize.cx == 0 && !CompletedFramesPending()) {
            // Drain the presenter once, but retain the NGX device/session.
            // Session::Generate already handles a genuine input-size change.
            StopMfg(false);
            m_mfg.ResetHistory();
            m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
            m_mfgInputModeKnown = false;
        }
        m_resizePending = true;
        m_resizeTarget = windowRect;
        m_resizeLastChangeTick = GetTickCount64();
        return S_OK;
    }

    m_resizePending = false;
    if (m_mfgMultiplier.load(std::memory_order_relaxed) && m_processingSize.cx == 0) {
        StopMfg(false);
        m_mfg.ResetHistory();
        m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
        m_mfgInputModeKnown = false;
    }
    m_windowRect = windowRect;
    UpdateRenderRect();
    HRESULT hr = S_OK;
    if (m_pDXGISwapChain1 && !m_bExclusiveScreen &&
        windowRect.Width() > 0 && windowRect.Height() > 0) {
        if (m_processingSize.cx > 0) DiscardMfgPreparedFrame();
        m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        hr = m_pDXGISwapChain1->ResizeBuffers(0,
            static_cast<UINT>(windowRect.Width()),
            static_cast<UINT>(windowRect.Height()), DXGI_FORMAT_UNKNOWN, 0);
    }
    UpdateStatsByWindow();
    UpdatePostScaleTexures();
    return hr;
}

HRESULT CDX11VideoProcessor::ApplyPendingWindowResize(bool settle)
{
    if (!m_resizePending) return S_OK;
    // 140 ms after the LAST change, not after the first. The presenter
    // continues to use the old DXGI buffers at source cadence meanwhile.
    constexpr ULONGLONG settleMs = 140;
    if (settle && m_pFilter->m_filterState == State_Running &&
        GetTickCount64() - m_resizeLastChangeTick < settleMs) return S_FALSE;

    const CRect previous = m_windowRect;
    const CRect finalRect = m_resizeTarget;
    m_completedGeometryTick = GetTickCount64();
    m_resizePending = false;
    m_windowRect = finalRect;
    if (m_pDXGISwapChain1 && !m_bExclusiveScreen &&
        finalRect.Width() > 0 && finalRect.Height() > 0) {
        if (m_processingSize.cx > 0) DiscardMfgPreparedFrame();
        m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        const HRESULT hr = m_pDXGISwapChain1->ResizeBuffers(0,
            static_cast<UINT>(finalRect.Width()),
            static_cast<UINT>(finalRect.Height()), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            // Do not run the renderer with mismatched viewport/buffer sizes.
            m_windowRect = previous;
            UpdateRenderRect();
            m_resizePending = true;
            m_resizeLastChangeTick = GetTickCount64();
            DLog(L"Deferred DXGI ResizeBuffers failed: {}", HR2Str(hr));
            return hr;
        }
    }
    UpdateRenderRect();
    UpdateStatsByWindow();
    UpdatePostScaleTexures();
    if (mfg::SourceCadenceTrace().Enabled()) {
        mfg::SourceCadenceTrace().Record("window_resize_commit", GetPreciseTick(), -1, -1,
            0, 0, 0, 0, 0, false, 0, S_OK);
    }
    DLog(L"Deferred DXGI resize committed: {}x{}", finalRect.Width(), finalRect.Height());
    return S_OK;
}

HRESULT CDX11VideoProcessor::Reset(bool bDisplayModeChange)
{
	DLog(L"CDX11VideoProcessor::Reset({})", bDisplayModeChange);

	CAutoLock cRendererLock(&m_HDRToggleLock);

	if (bDisplayModeChange) {
		if (m_bDisplayModeChangeAfterHDRToggle) {
			m_bDisplayModeChangeAfterHDRToggle = false;
			return S_OK;
		}
	}

	if ((m_bHdrPassthrough || m_bHdrLocalToneMapping) && SourceIsHDR()) {
		MONITORINFOEXW mi = { sizeof(mi) };
		GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi);
		DisplayConfig_t displayConfig = {};

		if (GetDisplayConfig(mi.szDevice, displayConfig)) {
			const auto bHdrPassthroughSupport = displayConfig.HDRSupported() && displayConfig.HDREnabled();
			if ((bHdrPassthroughSupport && !m_bHdrPassthroughSupport) || (!displayConfig.HDREnabled() && m_bHdrPassthroughSupport)) {
				m_bHDRModeChangeOutside = true;

				if (m_pFilter->m_inputMT.IsValid()) {
					CAutoLock cRendererLock(&m_pFilter->m_RendererLock);
					ReleaseSwapChain();

					if (m_iSwapEffect == SWAPEFFECT_Discard && !displayConfig.HDREnabled()) {
						m_pFilter->Init(true);
					} else {
						Init(m_hWnd, true);
					}
				}
			}
		}
	}

	return S_OK;
}

HRESULT CDX11VideoProcessor::GetCurentImage(long *pDIBImage)
{
	const bool projectJvr = CanProjectJvr();
	UINT w = m_srcRectWidth;
	UINT h = m_srcRectHeight;
	if (projectJvr) {
		w = jvr::FrameWidth(h);
	} else if (m_srcAnamorphic) {
		w = MulDiv(h, m_srcAspectRatioX, m_srcAspectRatioY);
	}
	if (m_iRotation == 90 || m_iRotation == 270) {
		std::swap(w, h);
	}
	const CRect imageRect(0, 0, w, h);

	const UINT dib_bitdepth = 32;
	const UINT dib_pitch    = CalcDibRowPitch(w, dib_bitdepth);

	BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)pDIBImage;
	ZeroMemory(pBIH, sizeof(BITMAPINFOHEADER));
	pBIH->biSize      = sizeof(BITMAPINFOHEADER);
	pBIH->biWidth     = w;
	pBIH->biHeight    = -(LONG)h; // top-down RGB bitmap
	pBIH->biPlanes    = 1;
	pBIH->biBitCount  = dib_bitdepth;
	pBIH->biSizeImage = dib_pitch * h;

	HRESULT hr = S_OK;
	CComPtr<ID3D11Texture2D> pRGB32Texture2D;
	D3D11_TEXTURE2D_DESC texdesc = CreateTex2DDesc(DXGI_FORMAT_B8G8R8X8_UNORM, w, h, Tex2D_DefaultRTarget);

	hr = m_pDevice->CreateTexture2D(&texdesc, nullptr, &pRGB32Texture2D);
	if (FAILED(hr)) {
		return hr;
	}

	const bool bHdrPassthrough = (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping) && (SourceIsHDR() || m_bVPUseRTXVideoHDR));
	const bool bSavedHdrPassthrough = m_bHdrPassthrough;
	const bool bSavedHdrLocalToneMapping = m_bHdrLocalToneMapping;

	if (bHdrPassthrough) {
		m_pPSHDR10ToneMapping.Release();
		m_pHDR10ToneMappingConstants.Release();

		if (m_D3D11VP.IsReady()) {
			m_pPSCorrection.Release();

			auto resId = (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_2084 || m_bVPUseRTXVideoHDR) ? IDF_PS_11_CONVERT_PQ_TO_SDR : IDF_PS_11_FIXCONVERT_HLG_TO_SDR;
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, resId));
			SetShaderLuminanceParams();
		} else {
			m_bHdrPassthrough = false;
			m_bHdrLocalToneMapping = false;
			UpdateConvertColorShader();
		}
	}

	const auto backupVidRect = m_videoRect;
	const auto backupWndRect = m_windowRect;
	m_videoRect  = imageRect;
	m_windowRect = imageRect;
	UpdateTexures();
	UpdatePostScaleTexures();

	auto pSub11CallBack = m_pFilter->m_pSub11CallBack;
	m_pFilter->m_pSub11CallBack = nullptr;

	if (m_completedLast && (!projectJvr || m_completedLast->pts != m_rtStart)) {
		hr = DrawCompletedVideo(*m_completedLast, pRGB32Texture2D, imageRect);
	} else {
		const auto sourceRect = ProcessingSourceRect(projectJvr);
		hr = projectJvr ? PrepareJvrFrame(sourceRect, false) : S_OK;
		if (SUCCEEDED(hr))
			hr = Process(pRGB32Texture2D, sourceRect, imageRect, false, false,
				INVALID_TIME, nullptr, nullptr, projectJvr ? &m_jvrFrame : nullptr);
	}

	m_pFilter->m_pSub11CallBack = pSub11CallBack;

	m_videoRect  = backupVidRect;
	m_windowRect = backupWndRect;
	UpdateTexures();
	UpdatePostScaleTexures();

	if (bHdrPassthrough) {
		if (m_D3D11VP.IsReady()) {
			m_pCorrectionConstants.Release();
			m_pPSCorrection.Release();

			if (m_srcExFmt.VideoTransferFunction == MFVideoTransFunc_HLG) {
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSCorrection, IDF_PS_11_CONVERT_HLG_TO_PQ));
			}
		} else {
			m_bHdrPassthrough = bSavedHdrPassthrough;
			m_bHdrLocalToneMapping = bSavedHdrLocalToneMapping;
			UpdateConvertColorShader();
		}

		if (m_bHdrPassthroughSupport && m_bHdrLocalToneMapping) {
			EXECUTE_ASSERT(S_OK == CreateHDR10ToneMappingShader());
			DLogIf(m_pPSHDR10ToneMapping, L"CDX11VideoProcessor::InitMediaType() m_pPSHDR10ToneMapping(type: '{}') created", m_iHdrLocalToneMappingType);
		}
	}

	if (FAILED(hr)) {
		return hr;
	}

	CComPtr<ID3D11Texture2D> pRGB32Texture2D_Shared;
	texdesc = CreateTex2DDesc(DXGI_FORMAT_B8G8R8X8_UNORM, w, h, Tex2D_StagingRead);

	hr = m_pDevice->CreateTexture2D(&texdesc, nullptr, &pRGB32Texture2D_Shared);
	if (FAILED(hr)) {
		return hr;
	}
	m_pDeviceContext->CopyResource(pRGB32Texture2D_Shared, pRGB32Texture2D);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (S_OK == m_pDeviceContext->Map(pRGB32Texture2D_Shared, 0, D3D11_MAP_READ, 0, &mr)) {
		CopyPlaneAsIs(h, (BYTE*)(pBIH + 1), dib_pitch, (BYTE*)mr.pData, mr.RowPitch);
		m_pDeviceContext->Unmap(pRGB32Texture2D_Shared, 0);
	} else {
		return E_FAIL;
	}

	return S_OK;
}

HRESULT CDX11VideoProcessor::GetDisplayedImage(BYTE **ppDib, unsigned* pSize)
{
	if (!m_pDXGISwapChain1 || !m_pDevice || !m_pDeviceContext) {
		return E_ABORT;
	}

	CComPtr<ID3D11Texture2D> pBackBuffer;
	HRESULT hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() failed with error {}", HR2Str(hr));
		return hr;
	}

	D3D11_TEXTURE2D_DESC desc;
	pBackBuffer->GetDesc(&desc);

	if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() backbuffer format not supported");
		return E_FAIL;
	}

	D3D11_TEXTURE2D_DESC desc2 = CreateTex2DDesc(desc.Format, desc.Width, desc.Height, Tex2D_StagingRead);
	CComPtr<ID3D11Texture2D> pTexture2DShared;
	hr = m_pDevice->CreateTexture2D(&desc2, nullptr, &pTexture2DShared);
	if (FAILED(hr)) {
		DLog(L"CDX11VideoProcessor::GetDisplayedImage() failed with error {}", HR2Str(hr));
		return hr;
	}

	m_pDeviceContext->CopyResource(pTexture2DShared, pBackBuffer);

	UINT dib_bitdepth;
	CopyFrameDataFn pConvertToDibFunc;
	if (desc2.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
		if (m_bAllowDeepColorBitmaps) {
			dib_bitdepth = 48;
			pConvertToDibFunc = ConvertR10G10B10A2toBGR48;
		} else {
			dib_bitdepth = 32;
			pConvertToDibFunc = ConvertR10G10B10A2toBGR32;
		}
	} else {
		dib_bitdepth = 32;
		pConvertToDibFunc = CopyPlaneAsIs;
	}
	const UINT dib_pitch    = CalcDibRowPitch(desc.Width, dib_bitdepth);
	const UINT dib_size     = dib_pitch * desc.Height;

	*pSize = sizeof(BITMAPINFOHEADER) + dib_size;
	BYTE* p = (BYTE*)LocalAlloc(LMEM_FIXED, *pSize); // only this allocator can be used
	if (!p) {
		return E_OUTOFMEMORY;
	}

	BITMAPINFOHEADER* pBIH = (BITMAPINFOHEADER*)p;
	ZeroMemory(pBIH, sizeof(BITMAPINFOHEADER));
	pBIH->biSize      = sizeof(BITMAPINFOHEADER);
	pBIH->biWidth     = desc.Width;
	pBIH->biHeight    = -(LONG)desc.Height; // top-down RGB bitmap
	pBIH->biBitCount  = dib_bitdepth;
	pBIH->biPlanes    = 1;
	pBIH->biSizeImage = dib_size;

	D3D11_MAPPED_SUBRESOURCE mappedResource = {};
	hr = m_pDeviceContext->Map(pTexture2DShared, 0, D3D11_MAP_READ, 0, &mappedResource);
	if (SUCCEEDED(hr)) {
		pConvertToDibFunc(desc.Height, (BYTE*)(pBIH + 1), dib_pitch, (BYTE*)mappedResource.pData, mappedResource.RowPitch);
		m_pDeviceContext->Unmap(pTexture2DShared, 0);
		*ppDib = p;
	} else {
		LocalFree(p);
	}

	return hr;
}

HRESULT CDX11VideoProcessor::GetVPInfo(std::wstring& str)
{
	str = L"DirectX 11";
	str += std::format(L"\nGraphics adapter: {}", m_strAdapterDescription);
	if (m_processingSize.cx > 0) {
		str += std::format(L"\nProcessing size : {}x{}; video destination {}x{}", m_processingSize.cx,
			m_processingSize.cy, m_videoRect.Width(), m_videoRect.Height());
	}
	if (m_iVPSuperRes == SUPERRES_DLSS_SR) {
		if (m_dlssSRInputStatus) {
			str += std::format(L"\nDLSS SR        : Inactive ({}); {}x{} to {}x{}, {}-bit, VP scaling {}",
				m_dlssSRInputStatus, m_srcRect.Width(), m_srcRect.Height(),
				m_videoRect.Width(), m_videoRect.Height(), m_srcParams.CDepth, m_bVPScaling);
		} else {
			str += std::format(L"\nDLSS SR        : {}", m_dlssSR.Status(UsesZipDepthSr() ? &m_zipDepth : nullptr));
			if (m_dlssSR.Active()) {
				str += m_dlssSRAfterMfg ? L" (after MFG)" : L" (source frames)";
				if (m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) str += L" (deinterlaced)";
				const auto stats = m_dlssSR.GetStats();
				if (stats.ngxGpuTimeValid) str += std::format(L"\nDLSS SR NGX GPU: {:.2f} ms", stats.ngxGpuMs);
			}
		}
	}
	if (UsesZipDepthSr() || UsesZipDepthMfg()) {
		const auto& depth = m_zipDepth.GetCounters();
		const auto latest = m_zipDepth.LatestPreview();
		str += std::format(L"\nZipDepth       : {} captures, {} DirectML jobs, {} maps ({} late)"
			L"\nZipDepth usage : {} SR evaluations, {} MFG submissions",
			depth.captures, depth.jobs, depth.maps, depth.lateMaps, depth.srEvaluations, depth.mfgSubmissions);
		if (const auto* precision = m_zipDepth.ModelPrecision(); *precision)
			str += std::format(L"\nZipDepth model : {}", precision);
		if (depth.maps)
			str += std::format(L"\nZipDepth timing: GPU job {:.1f} ms, ready at {:.0f} ms age",
				depth.inferenceMs, double(depth.resultAge) / 10000.0);
		if (depth.gpuTimings.available)
			str += std::format(L"\nZipDepth GPU   : pack {:.2f} ms, model interval {:.2f} ms, post {:.2f} ms",
				depth.gpuTimings.packMs, depth.gpuTimings.modelMs, depth.gpuTimings.postMs);
		if (latest.view)
			str += std::format(L"\nZipDepth latest: {:.0f} ms PTS", double(latest.pts) / 10000.0);
		else
			str += std::format(L"\nZipDepth latest: {}", m_zipDepth.Status());
	}
	str.append(L"\nVideoProcessor  : ");
	if (m_D3D11VP.IsReady()) {
		D3D11_VIDEO_PROCESSOR_CAPS caps;
		UINT rateConvIndex;
		D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rateConvCaps;
		m_D3D11VP.GetVPParams(caps, rateConvIndex, rateConvCaps);

		str += std::format(L"D3D11, RateConversion_{}", rateConvIndex);

		str.append(L"\nDeinterlaceTech.:");
		if (rateConvCaps.ProcessorCaps) {
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND)               str.append(L" Blend,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB)                 str.append(L" Bob,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE)            str.append(L" Adaptive,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION) str.append(L" Motion Compensation,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE)                str.append(L" Inverse Telecine,");
			if (rateConvCaps.ProcessorCaps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION)           str.append(L" Frame Rate Conversion");
		} else {
			str.append(L" none");
		}
		str_trim_end(str, ',');
		str += std::format(L"\nReference Frames: Past {}, Future {}", rateConvCaps.PastFrames, rateConvCaps.FutureFrames);
	} else {
		str.append(L"Shaders");
	}

	str.append(m_strStatsDispInfo);

	if (m_pPostScaleShaders.size()) {
		str.append(L"\n\nPost scale pixel shaders:");
		for (const auto& pshader : m_pPostScaleShaders) {
			str += std::format(L"\n  {}", pshader.name);
		}
	}

#ifdef _DEBUG
	str.append(L"\n\nDEBUG info:");
	str += std::format(L"\nSource tex size: {}x{}", m_srcWidth, m_srcHeight);
	str += std::format(L"\nSource rect    : {},{},{},{} - {}x{}", m_srcRect.left, m_srcRect.top, m_srcRect.right, m_srcRect.bottom, m_srcRect.Width(), m_srcRect.Height());
	str += std::format(L"\nVideo rect     : {},{},{},{} - {}x{}", m_videoRect.left, m_videoRect.top, m_videoRect.right, m_videoRect.bottom, m_videoRect.Width(), m_videoRect.Height());
	str += std::format(L"\nWindow rect    : {},{},{},{} - {}x{}", m_windowRect.left, m_windowRect.top, m_windowRect.right, m_windowRect.bottom, m_windowRect.Width(), m_windowRect.Height());

	if (m_pDevice) {
		std::vector<std::pair<const DXGI_FORMAT, UINT>> formatsYUV = {
			{ DXGI_FORMAT_NV12,               0 },
			{ DXGI_FORMAT_P010,               0 },
			{ DXGI_FORMAT_P016,               0 },
			{ DXGI_FORMAT_YUY2,               0 },
			{ DXGI_FORMAT_Y210,               0 },
			{ DXGI_FORMAT_Y216,               0 },
			{ DXGI_FORMAT_AYUV,               0 },
			{ DXGI_FORMAT_Y410,               0 },
			{ DXGI_FORMAT_Y416,               0 },
		};
		std::vector<std::pair<const DXGI_FORMAT, UINT>> formatsRGB = {
			{ DXGI_FORMAT_B8G8R8X8_UNORM,     0 },
			{ DXGI_FORMAT_B8G8R8A8_UNORM,     0 },
			{ DXGI_FORMAT_R10G10B10A2_UNORM,  0 },
			{ DXGI_FORMAT_R16G16B16A16_UNORM, 0 },
		};
		for (auto& [format, formatSupport] : formatsYUV) {
			m_pDevice->CheckFormatSupport(format, &formatSupport);
		}
		for (auto& [format, formatSupport] : formatsRGB) {
			m_pDevice->CheckFormatSupport(format, &formatSupport);
		}

		int count = 0;
		str += L"\nD3D11 VP input formats  :";
		for (const auto& [format, formatSupport] : formatsYUV) {
			if (formatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
				count++;
			}
		}
		if (count) {
			str += L"\n ";
		}
		for (const auto& [format, formatSupport] : formatsRGB) {
			if (formatSupport & D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
			}
		}

		count = 0;
		str += L"\nShader Texture2D formats:";
		for (const auto& [format, formatSupport] : formatsYUV) {
			if (formatSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
				count++;
			}
		}
		if (count) {
			str += L"\n ";
		}
		for (const auto& [format, formatSupport] : formatsRGB) {
			if (formatSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
				str.append(L" ");
				str.append(DXGIFormatToString(format));
			}
		}
	}
#endif

	return S_OK;
}

void CDX11VideoProcessor::Configure(const Settings_t& requested)
{
	Settings_t config = requested;
	mfg::EnforceScope(config);
	const bool jvrFlatView = m_pFilter->JvrProjectionRequested(config);
	ApplyJvrPlaybackMode(config, jvrFlatView);
	const int superRes = config.bDlssSR ? SUPERRES_DLSS_SR : config.iVPSuperRes;
	const unsigned mfgMultiplier = config.bUseD3D11 ? config.iMfgMultiplier : 0;
	const bool srWasEnabled = m_iVPSuperRes == SUPERRES_DLSS_SR;
	const bool srChanged = srWasEnabled != config.bDlssSR ||
		(config.bDlssSR && m_dlssSRSourceLimit != config.iDlssSRSourceLimit);
	const bool wasUsingZipDepth = UsesZipDepthSr() || UsesZipDepthMfg();
	const bool useZipDepth = (config.bUseZipDepthSr && config.bDlssSR) ||
		(config.bUseZipDepthMfg && mfgMultiplier);
	const bool overlayChanged = (wasUsingZipDepth && m_showDepthOverlay) !=
		(useZipDepth && config.bShowDepthOverlay);
	const unsigned processingMode = config.iMfgProcessingMode <= 1 ? config.iMfgProcessingMode : 0;
	const bool jvrModeChanged = jvrFlatView != m_jvrFlatView;
	if (processingMode != m_mfgProcessingMode ||
		(m_mfgMultiplier && (srChanged || overlayChanged || jvrModeChanged) && m_pFilter->GetActive())) {
		SwitchMfgUpscaleSlot(processingMode);
	} else if (overlayChanged) {
		m_dlssSR.ResetHistory();
	}
	if (config.bShowFeatureBadges &&
		(!m_showFeatureStatus.load(std::memory_order_relaxed) ||
		 m_mfgMultiplier.load(std::memory_order_relaxed) != mfgMultiplier ||
		 m_iVPSuperRes != superRes || srChanged)) {
		ResetFeatureStatus();
	}
	m_jvrFlatView = jvrFlatView;
	ConfigureJvrProjection(config);
	if (jvrModeChanged) {
		m_zipDepth.StopLookahead();
		m_zipDepth.ResetHistory();
		if (!jvrFlatView) {
			m_jvrSource.Release();
			m_jvrFrame.Release();
		}
	}
	m_rtVideoDelay=config.iVideoDelay*10000LL;
	// MFG cannot remain active on an existing D3D11 instance after the
	// user disables D3D11. Preserve the saved selection in Settings_t.
	if (m_mfgLowMotionEnabled != config.bMfgLowMotion) {
		if (m_mfgPendingRepeats.load(std::memory_order_relaxed) || m_mfgRepeatHold) m_mfg.ResetHistory();
		ResetMfgFrameAnalysis();
		m_mfgRepeatTiming.ClearEvidence();
		m_mfgDecodedDifference = {};
		m_mfgRepeatsBypassed = 0;
		m_mfgLowMotionSaved = 0;
		m_mfgLowMotionVetoes = 0;
		m_mfgLowMotionEnabled = config.bMfgLowMotion;
	}
	if (m_mfgMultiplier != mfgMultiplier) {
		// There is no MFG session to stop when enabling for the first time.
		// NGX initialization stays lazy: QueueMfg() is entered only after a
		// valid video frame arrives on the active DirectShow render path.
		if (m_mfgMultiplier) {
			if (mfgMultiplier && !m_mfg.Failed()) {
				StopMfg(false);
				m_mfg.ResetHistory();
			} else {
				StopMfg();
			}
		}
		m_mfgMultiplier = mfgMultiplier;
		m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
	}
	SetSvdLutStrength(config.GetSvdLutStrength());
	if (jvrModeChanged) ResetSvdLutHistory();
	if (m_useZipDepthSr != config.bUseZipDepthSr) m_dlssSR.ResetHistory();
	if (m_useZipDepthMfg != config.bUseZipDepthMfg || m_adaptiveMfgDepth != config.bAdaptiveMfgDepth)
		m_mfg.ResetDepthHistory();
	m_useZipDepthSr = config.bUseZipDepthSr;
	m_useZipDepthMfg = config.bUseZipDepthMfg;
	m_adaptiveMfgDepth = config.bAdaptiveMfgDepth;
	m_showMfgMotion = config.bShowMfgMotion;
	m_showDepthOverlay = config.bShowDepthOverlay;
	m_depth3DPreview = config.bDepth3DPreview;
	if (!m_showDepthOverlay || !useZipDepth) m_depthOverlayCapture.Release();
	if (wasUsingZipDepth && !useZipDepth) m_zipDepth.Stop();
	bool changeWindow            = false;
	bool changeDevice            = false;
	bool changeVP                = false;
	bool changeHDR               = false;
	bool changeTextures          = false;
	bool changeConvertShader     = false;
	bool changeBitmapShader      = false;
	bool changeUpscalingShader   = false;
	bool changeDowndcalingShader = false;
	bool changeNumTextures       = false;
	bool changeResizeStats       = false;
	bool changeSuperRes          = false;
	bool changeRTXVideoHDR       = false;
	bool changeLuminanceParams   = false;

	// settings that do not require preparation
	m_bShowStats           = config.bShowStats;
	m_showFeatureStatus.store(config.bShowFeatureBadges, std::memory_order_relaxed);
	m_bDeintDouble         = config.bDeintDouble;
	m_bInterpolateAt50pct  = config.bInterpolateAt50pct;
	m_bVBlankBeforePresent = config.bVBlankBeforePresent;
	m_bAdjustPresentTime   = config.bAdjustPresentTime;
	m_bDeintBlend          = config.bDeintBlend;

	// checking what needs to be changed

	if (config.iResizeStats != m_iResizeStats) {
		m_iResizeStats = config.iResizeStats;
		changeResizeStats = true;
	}

	if (config.iTexFormat != m_iTexFormat) {
		m_iTexFormat = config.iTexFormat;
		changeTextures = true;
	}

	if (m_srcParams.cformat == CF_NV12) {
		changeVP = config.VPFmts.bNV12 != m_VPFormats.bNV12;
	}
	else if (m_srcParams.cformat == CF_P010 || m_srcParams.cformat == CF_P016) {
		changeVP = config.VPFmts.bP01x != m_VPFormats.bP01x;
	}
	else if (m_srcParams.cformat == CF_YUY2) {
		changeVP = config.VPFmts.bYUY2 != m_VPFormats.bYUY2;
	}
	else {
		changeVP = config.VPFmts.bOther != m_VPFormats.bOther;
	}
	m_VPFormats = config.VPFmts;

	if (config.iVPDeinterlacing != m_iVPDeinterlacing) {
		m_iVPDeinterlacing = config.iVPDeinterlacing;
		if (m_bInterlaced) {
			changeVP = true;
		}
	}

	if (config.bVPScaling != m_bVPScaling) {
		m_bVPScaling = config.bVPScaling;
		changeTextures = true;
		changeVP = true; // temporary solution
	}

	if (config.iChromaScaling != m_iChromaScaling) {
		m_iChromaScaling = config.iChromaScaling;
		changeConvertShader = m_PSConvColorData.bEnable && (m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422);
	}

	if (config.iHdrOsdBrightness != m_iHdrOsdBrightness) {
		m_iHdrOsdBrightness = config.iHdrOsdBrightness;
		changeBitmapShader = true;
	}

	if (config.iUpscaling != m_iUpscaling) {
		m_iUpscaling = config.iUpscaling;
		changeUpscalingShader = true;
	}
	if (config.iDownscaling != m_iDownscaling) {
		m_iDownscaling = config.iDownscaling;
		changeDowndcalingShader = true;
	}

	if (config.bUseDither != m_bUseDither) {
		m_bUseDither = config.bUseDither;
		changeNumTextures = m_InternalTexFmt != DXGI_FORMAT_B8G8R8A8_UNORM;
	}

	if (config.iSwapEffect != m_iSwapEffect) {
		m_iSwapEffect = config.iSwapEffect;
		changeWindow = !m_pFilter->m_bExclusiveScreen;
	}

	if (config.bHdrPreferDoVi != m_bHdrPreferDoVi) {
		if (m_Dovi.bValid && !config.bHdrPreferDoVi && SourceIsHDR10orHLG()) {
			m_Dovi = {};
			m_DoviExtensionMetadata = {};
			changeVP = true;
		}
		m_bHdrPreferDoVi = config.bHdrPreferDoVi;
	}

	if (config.bHdrPassthrough != m_bHdrPassthrough) {
		m_bHdrPassthrough = config.bHdrPassthrough;
		changeHDR = true;
	}

	if (config.bHdrLocalToneMapping != m_bHdrLocalToneMapping ||
			config.iHdrLocalToneMappingType != m_iHdrLocalToneMappingType ||
			config.bHdrLocalMeasurePeak != m_bHdrLocalMeasurePeak ||
			config.iHdrDoviToneMappingType != m_iHdrDoviToneMappingType) {
		m_bHdrLocalToneMapping = config.bHdrLocalToneMapping;
		m_iHdrLocalToneMappingType = config.iHdrLocalToneMappingType;
		m_bHdrLocalMeasurePeak = config.bHdrLocalMeasurePeak;
		m_iHdrDoviToneMappingType = config.iHdrDoviToneMappingType;
		changeHDR = true;
	}

	if (config.iHdrDisplayMaxNits != m_iHdrDisplayMaxNits) {
		m_iHdrDisplayMaxNits = config.iHdrDisplayMaxNits;
		//changeHDR = true;
	}

	if (config.iHdrToggleDisplay != m_iHdrToggleDisplay) {
		if (config.iHdrToggleDisplay == HDRTD_Disabled || m_iHdrToggleDisplay == HDRTD_Disabled) {
			changeHDR = true;
		}
		m_iHdrToggleDisplay = config.iHdrToggleDisplay;
	}

	if ((config.iSdrToneMappingType != SDRTM_Fixed) != m_bSdrToneMapping) {
		m_bSdrToneMapping = config.iSdrToneMappingType != SDRTM_Fixed;
		changeHDR = true; // the conversion shader carries the curve
	}
	if ((config.iSdrToneMappingType == SDRTM_Measured) != m_bSdrMeasurePeak) {
		m_bSdrMeasurePeak = config.iSdrToneMappingType == SDRTM_Measured;
		changeHDR = true; // picks the measuring variant of the conversion shader, or not
	}
	// read every frame by the measurement, so nothing needs rebuilding
	m_iSdrPeakWindowMs = config.iSdrPeakWindowMs;
	m_iSdrPeakFloorNits = config.iSdrPeakFloorNits;
	m_bSdrPeakSceneCuts = config.bSdrPeakSceneCuts;

	if (config.bConvertToSdr != m_bConvertToSdr) {
		m_bConvertToSdr = config.bConvertToSdr;
		if (SourceIsHDR()) {
			if (m_D3D11VP.IsReady()) {
				changeNumTextures = true;
				changeVP = true; // temporary solution
			} else {
				changeConvertShader = true;
			}
		}
	}

	if (srChanged) {
		StopMfg(false);
		m_mfg.ResetHistory();
		m_dlssSR.Reset();
		m_dlssSRSource.Release();
	}
    m_dlssSRSourceLimit = config.iDlssSRSourceLimit;
	m_dlssSRDlaa = config.bDlssSRDlaa;
	if (superRes != m_iVPSuperRes) {
		m_iVPSuperRes = superRes;
		// A live JVR toggle can keep the decoded frame when the existing
		// VP output is suitable for the selected mode.
		const bool need8BitVp = superRes != SUPERRES_Disable && m_srcParams.CDepth == 8 &&
			!SourceIsHDR() && m_D3D11OutputFmt != DXGI_FORMAT_B8G8R8A8_UNORM;
		if (m_VendorId == PCIV_NVIDIA && (!jvrModeChanged || need8BitVp)) changeVP = true;
		changeSuperRes = true;
	}

	if (config.bVPRTXVideoHDR != m_bVPRTXVideoHDR) {
		m_bVPRTXVideoHDR = config.bVPRTXVideoHDR;
		changeRTXVideoHDR = true;
	}

	if (config.iSDRDisplayNits != m_iSDRDisplayNits) {
		m_iSDRDisplayNits = config.iSDRDisplayNits;
		if (SourceIsHDR()) {
			changeLuminanceParams = true;
		}
	}

	if (!m_pFilter->GetActive()) {
		return;
	}

	// apply new settings

	if (changeWindow) {
		ReleaseSwapChain();
		EXECUTE_ASSERT(S_OK == m_pFilter->Init(true));

		if (changeHDR && (SourceIsHDR10orHLG() || m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR) || m_iHdrToggleDisplay) {
			m_srcVideoTransferFunction = 0;
			InitMediaType(&m_pFilter->m_inputMT);
		}
		return;
	}

	if (changeHDR) {
		if (SourceIsHDR10orHLG() || m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR || m_iHdrToggleDisplay) {
			if (m_iSwapEffect == SWAPEFFECT_Discard) {
				ReleaseSwapChain();
				m_pFilter->Init(true);
			}

			m_srcVideoTransferFunction = 0;
			InitMediaType(&m_pFilter->m_inputMT);

			return;
		}
	}

	if (m_Dovi.bValid) {
		changeVP = false;
	}
	if (changeVP) {
		InitMediaType(&m_pFilter->m_inputMT);
		if (m_bVPUseRTXVideoHDR || m_bVPRTXVideoHDR) {
			InitSwapChain(false);
		}

		return; // need some test
	}

	if (changeRTXVideoHDR) {
		InitMediaType(&m_pFilter->m_inputMT);

		return;
	}

	// changes that do not require a global rebuild of the video processor

	if (changeTextures) {
		UpdateTexParams(m_srcParams.CDepth);
		if (m_D3D11VP.IsReady()) {
			// update m_D3D11OutputFmt
			EXECUTE_ASSERT(S_OK == InitializeD3D11VP(m_srcParams, m_srcWidth, m_srcHeight, &m_pFilter->m_inputMT));
		}
		UpdateTexures();
		UpdatePostScaleTexures();
	}

	if (changeConvertShader) {
		UpdateConvertColorShader();
	}

	if (changeBitmapShader) {
		UpdateBitmapShader();
	}

	if (changeUpscalingShader) {
		UpdateUpscalingShaders();
	}
	if (changeDowndcalingShader) {
		UpdateDownscalingShaders();
	}

	if (changeLuminanceParams) {
		SetShaderLuminanceParams();
	}

	if (changeNumTextures) {
		UpdatePostScaleTexures();
	}

	if (changeResizeStats) {
		UpdateStatsByWindow();
		UpdateStatsByDisplay();
	}

	if (changeSuperRes) {
		auto superRes = (m_bVPScaling && (m_srcParams.CDepth == 8 || !m_bACMEnabled)) ? m_iVPSuperRes : SUPERRES_Disable;
		m_bVPUseSuperRes = (m_D3D11VP.SetSuperRes(superRes) == S_OK);
	}

	UpdateStatsStatic();
}

void CDX11VideoProcessor::SetRotation(int value)
{
	m_iRotation = value;
	if (m_D3D11VP.IsReady()) {
		m_D3D11VP.SetRotation(static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(value / 90));
	}
}

void CDX11VideoProcessor::SetStereo3dTransform(int value)
{
	m_iStereo3dTransform = value;

	if (m_iStereo3dTransform == 1) {
		if (!m_pPSHalfOUtoInterlace) {
			EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPSHalfOUtoInterlace, IDF_PS_11_HALFOU_TO_INTERLACE));
		}
	}
	else {
		m_pPSHalfOUtoInterlace.Release();
	}
}

void CDX11VideoProcessor::Flush()
{
	if (m_pFilter->m_filterState == State_Stopped) {
		m_startupHasPicture = false;
		m_extendedStartupPending = false;
		m_mfgDisplayRate = {};
	} else {
		m_mfgDisplayRate.BreakContinuity();
	}
	ClearCompletedFrames(false);
	ResetSvdLutHistory();
	m_zipDepth.StopLookahead();
	m_zipDepth.ResetHistory();
	m_dlssSR.ResetHistory();
	StopMfg(false);m_mfg.ResetHistory();
	m_mfgAutoEpoch = m_mfgAutoLastPts = -1;
	if (m_D3D11VP.IsReady()) {
		m_D3D11VP.ResetFrameOrder();
	}

	m_rtStart = m_sampleDuration = 0;

	m_DoviExtensionMetadata = {};
	m_lastDoViDynamicConstantsBuffer = {};
#ifndef NDEBUG
	UpdateStatsStatic();
#endif
}

void CDX11VideoProcessor::ClearPreScaleShaders()
{
	for (auto& pExtShader : m_pPreScaleShaders) {
		pExtShader.shader.Release();
	}
	m_pPreScaleShaders.clear();
	DLog(L"CDX11VideoProcessor::ClearPreScaleShaders().");
}


void CDX11VideoProcessor::ClearPostScaleShaders()
{
	for (auto& pExtShader : m_pPostScaleShaders) {
		pExtShader.shader.Release();
	}
	m_pPostScaleShaders.clear();
	//UpdateStatsPostProc();
	DLog(L"CDX11VideoProcessor::ClearPostScaleShaders().");
}

HRESULT CDX11VideoProcessor::AddPreScaleShader(const std::wstring& name, const std::string& srcCode)
{
#ifdef _DEBUG
	if (!m_pDevice) {
		return E_ABORT;
	}

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = CompileShader(srcCode, nullptr, "ps_4_0", &pShaderCode);
	if (S_OK == hr) {
		m_pPreScaleShaders.emplace_back();
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPreScaleShaders.back().shader);
		if (S_OK == hr) {
			m_pPreScaleShaders.back().name = name;
			//UpdatePreScaleTexures(); //TODO
			DLog(L"CDX11VideoProcessor::AddPreScaleShader() : \"{}\" pixel shader added successfully.", name);
		}
		else {
			DLog(L"CDX11VideoProcessor::AddPreScaleShader() : create pixel shader \"{}\" FAILED!", name);
			m_pPreScaleShaders.pop_back();
		}
		pShaderCode->Release();
	}

	if (S_OK == hr && m_D3D11VP.IsReady() && m_bVPScaling) {
		return S_FALSE;
	}

	return hr;
#else
	return E_NOTIMPL;
#endif
}

HRESULT CDX11VideoProcessor::AddPostScaleShader(const std::wstring& name, const std::string& srcCode)
{
	if (!m_pDevice) {
		return E_ABORT;
	}

	ID3DBlob* pShaderCode = nullptr;
	HRESULT hr = CompileShader(srcCode, nullptr, "ps_4_0", &pShaderCode);
	if (S_OK == hr) {
		m_pPostScaleShaders.emplace_back();
		hr = m_pDevice->CreatePixelShader(pShaderCode->GetBufferPointer(), pShaderCode->GetBufferSize(), nullptr, &m_pPostScaleShaders.back().shader);
		if (S_OK == hr) {
			m_pPostScaleShaders.back().name = name;
			UpdatePostScaleTexures();
			DLog(L"CDX11VideoProcessor::AddPostScaleShader() : \"{}\" pixel shader added successfully.", name);
		}
		else {
			DLog(L"CDX11VideoProcessor::AddPostScaleShader() : create pixel shader \"{}\" FAILED!", name);
			m_pPostScaleShaders.pop_back();
		}
		pShaderCode->Release();
	}

	return hr;
}

ISubPicAllocator* CDX11VideoProcessor::GetSubPicAllocator()
{
	if (!m_pSubPicAllocator && m_pDevice) {
		m_pSubPicAllocator = new CDX11SubPicAllocator(m_pDevice, { 1280, 720 });
	}
	return m_pSubPicAllocator;
}

void CDX11VideoProcessor::UpdateStatsPresent()
{
	DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
	if (m_pDXGISwapChain1 && S_OK == m_pDXGISwapChain1->GetDesc1(&swapchain_desc)) {
		m_strStatsPresent.assign(L"\nPresentation  : ");
		switch (swapchain_desc.SwapEffect) {
		case DXGI_SWAP_EFFECT_DISCARD:
			m_strStatsPresent.append(L"Discard");
			break;
		case DXGI_SWAP_EFFECT_SEQUENTIAL:
			m_strStatsPresent.append(L"Sequential");
			break;
		case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
			m_strStatsPresent.append(L"Flip sequential");
			break;
		case DXGI_SWAP_EFFECT_FLIP_DISCARD:
			m_strStatsPresent.append(L"Flip discard");
			break;
		}
		m_strStatsPresent.append(L", ");
		m_strStatsPresent.append(DXGIFormatToString(swapchain_desc.Format));

		if ((m_bVBlankBeforePresent && m_pDXGIOutput) || m_bAdjustPresentTime) {
			m_strStatsPresent.append(L"\nFrame sync    :");
			if (m_bVBlankBeforePresent && m_pDXGIOutput) {
				m_strStatsPresent.append(L" wait VBlank");
			}
			if (m_bAdjustPresentTime) {
				if (m_strStatsPresent.back() != ':') {
					m_strStatsPresent += ',';
				}
				m_strStatsPresent.append(L" adjust present time");
			}
		}
	}
}

void CDX11VideoProcessor::UpdateStatsStatic()
{
	if (!m_bShowStats) {
		return;
	}

	if (m_srcParams.cformat) {
		m_strStatsHeader = std::format(L"MPC VR {}, Direct3D 11, Windows {}", _CRT_WIDE(VERSION_STR), GetWindowsVersion());

		UpdateStatsInputFmt();

		m_strStatsVProc.assign(L"\nVideoProcessor: ");
		if (m_D3D11VP.IsReady()) {
			m_strStatsVProc += std::format(L"D3D11 VP, output to {}", DXGIFormatToString(m_D3D11OutputFmt));
		} else {
			m_strStatsVProc.append(L"Shaders");
			if (m_srcParams.Subsampling == 420 || m_srcParams.Subsampling == 422) {
				m_strStatsVProc.append(L", Chroma scaling: ");
				switch (m_iChromaScaling) {
				case CHROMA_Nearest:
					m_strStatsVProc.append(L"Nearest-neighbor");
					break;
				case CHROMA_Bilinear:
					m_strStatsVProc.append(L"Bilinear");
					break;
				case CHROMA_CatmullRom:
					m_strStatsVProc.append(L"Catmull-Rom");
					break;
				}
			}
		}
		m_strStatsVProc += std::format(L"\nInternalFormat: {}", DXGIFormatToString(m_InternalTexFmt));

		if (SourceIsHDR() || m_bVPUseRTXVideoHDR) {
			m_strStatsHDR.assign(L"\nHDR processing: ");
			if (m_bHdrPassthroughSupport && (m_bHdrPassthrough || m_bHdrLocalToneMapping)) {
				if (m_bHdrPassthrough || m_bVPUseRTXVideoHDR) {
					m_strStatsHDR.append(L"Passthrough");
				} else if (m_bHdrLocalToneMapping) {
					m_strStatsHDR.append(L"Local tone mapping:");
					switch (HdrLocalToneMappingType()) {
						case 1:
							m_strStatsHDR.append(L" ACES");
							break;
						case 2:
							m_strStatsHDR.append(L" Reinhard");
							break;
						case 3:
							m_strStatsHDR.append(L" Habel");
							break;
						case 4:
							m_strStatsHDR.append(L" Mobius");
							break;
						case 5:
							m_strStatsHDR.append(L" BT2390");
							break;
						case 6:
							m_strStatsHDR.append(L" ST 2094-10");
							break;
						case 7:
							m_strStatsHDR.append(L" Spline");
							break;
						default:
							break;
					}
					if (m_bHdrLocalMeasureActive) {
						m_strStatsHDR.append(L", measured");
					}
					m_strStatsHDR.append(std::format(L"\n Display Max Nits: {} nits", m_iHdrDisplayMaxNits));
				}
				if (m_bVPUseRTXVideoHDR) {
					m_strStatsHDR.append(L", RTX Video HDR*");
				}
				if (m_bHdrLocalToneMapping && m_DoviExtensionMetadata.L1.present) {
					m_strStatsHDR += std::format(L", {} nits", m_DoviExtensionMetadata.L1.max_pq);
#ifndef NDEBUG
					if (m_DoviExtensionMetadata.L1.present || m_DoviExtensionMetadata.L2.present) {
						m_strStatsHDR.append(L"\n Dolby Vision metadata:");

						if (m_DoviExtensionMetadata.L1.present) {
							m_strStatsHDR.append(std::format(L"\n  L1: min/max/avg pq : {}/{}/{}",
															 m_DoviExtensionMetadata.L1.min_pq,
															 m_DoviExtensionMetadata.L1.max_pq,
															 m_DoviExtensionMetadata.L1.avg_pq));
						}

						if (m_DoviExtensionMetadata.L2.present) {
							m_strStatsHDR.append(
								std::format(L"\n  L2: trim_slope: {:.2f}\n      trim_offset: {:.2f}\n      trim_power: {:.2f}\n      trim_saturation_gain: {:.2f}\n      trim_chroma_weight: {:.2f}",
											m_DoviExtensionMetadata.L2.trim_slope,
											m_DoviExtensionMetadata.L2.trim_offset,
											m_DoviExtensionMetadata.L2.trim_power,
											m_DoviExtensionMetadata.L2.trim_saturation_gain,
											m_DoviExtensionMetadata.L2.trim_chroma_weight));
						}
					}
#endif
				} else if (m_lastHdr10.bValid) {
					if (m_bHdrLocalToneMapping) {
						m_strStatsHDR += std::format(L"\n HDR10 metadata  : Max Luminance: {}", m_lastHdr10.hdr10.MaxMasteringLuminance);
						if (m_lastHdr10.hdr10.MaxContentLightLevel) {
							m_strStatsHDR += std::format(L", MaxCLL: {}", m_lastHdr10.hdr10.MaxContentLightLevel);
						}
						m_strStatsHDR += L" nits";
					} else {
						m_strStatsHDR += std::format(L", {} nits", m_lastHdr10.hdr10.MaxMasteringLuminance);
					}
				}
			} else if (m_bConvertToSdr) {
				m_strStatsHDR.append(L"Convert to SDR");
#ifndef NDEBUG
				if (m_DoviExtensionMetadata.L2.present) {
					m_strStatsHDR.append(L"\n Dolby Vision metadata:");
					m_strStatsHDR.append(
						std::format(L"\n  L2: trim_slope: {:.2f}\n      trim_offset: {:.2f}\n      trim_power: {:.2f}\n      trim_saturation_gain: {:.2f}\n      trim_chroma_weight: {:.2f}",
									m_DoviExtensionMetadata.L2.trim_slope,
									m_DoviExtensionMetadata.L2.trim_offset,
									m_DoviExtensionMetadata.L2.trim_power,
									m_DoviExtensionMetadata.L2.trim_saturation_gain,
									m_DoviExtensionMetadata.L2.trim_chroma_weight));
				}
#endif
			} else {
				m_strStatsHDR.append(L"Not used");
			}
		} else {
			m_strStatsHDR.clear();
		}

		UpdateStatsPresent();
	}
	else {
		m_strStatsHeader = L"Error";
		m_strStatsVProc.clear();
		m_strStatsInputFmt.clear();
		//m_strStatsPostProc.clear();
		m_strStatsHDR.clear();
		m_strStatsPresent.clear();
	}
}

/*
void CDX11VideoProcessor::UpdateStatsPostProc()
{
	if (m_strCorrection || m_pPostScaleShaders.size() || m_bFinalPass) {
		m_strStatsPostProc.assign(L"\nPostProcessing:");
		if (m_strCorrection) {
			m_strStatsPostProc += std::format(L" {},", m_strCorrection);
		}
		if (m_pPostScaleShaders.size()) {
			m_strStatsPostProc += std::format(L" shaders[{}],", m_pPostScaleShaders.size());
		}
		if (m_bFinalPass) {
			m_strStatsPostProc.append(L" dither");
		}
		str_trim_end(m_strStatsPostProc, ',');
	}
	else {
		m_strStatsPostProc.clear();
	}
}
*/


void CDX11VideoProcessor::ResetFeatureStatus()
{
	std::lock_guard guard(m_featureStatusMutex);
	m_featureStatusUntil = 0;
	m_featureStatusLastGenerated = m_mfgGeneratedPresents.load(std::memory_order_relaxed);
	m_featureStatusGeneratedTick = 0;
	m_featureStatusMfgActive = false;
}

LONG CDX11VideoProcessor::DrawFeatureStatus(ID3D11Texture2D* target)
{
	if (!target || !m_pDevice || !m_showFeatureStatus.load(std::memory_order_relaxed)) {
		return 0;
	}

	const auto state = m_pFilter->m_filterState;
	if (state == State_Stopped) return 0;
	const bool paused = state == State_Paused;
	D3D11_TEXTURE2D_DESC desc{};
	target->GetDesc(&desc);
	const CSize size(static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height));
	const ULONGLONG now = GetTickCount64();
	bool mfgActive;
	{
		std::lock_guard guard(m_featureStatusMutex);
		const auto playbackRun = m_pFilter->m_featureStatusPlaybackRun.load(std::memory_order_relaxed);
		if (m_featureStatusSize != size || m_featureStatusPlaybackRun != playbackRun || paused) {
			m_featureStatusSize = size;
			m_featureStatusPlaybackRun = playbackRun;
			m_featureStatusUntil = 0;
		}
		const auto generated = m_mfgGeneratedPresents.load(std::memory_order_relaxed);
		if (generated != m_featureStatusLastGenerated) {
			m_featureStatusLastGenerated = generated;
			m_featureStatusGeneratedTick = now;
		}
		const bool mfgPresentedRecently = m_featureStatusGeneratedTick && now - m_featureStatusGeneratedTick <= 2000;
		if (!paused) {
			m_featureStatusMfgActive = mfgPresentedRecently &&
				m_mfgEffectiveMultiplier.load(std::memory_order_relaxed) >= 2;
		}
		// A paused picture can keep the current MFG label even though no new
		// generated frames are presented. Preroll may already have warmed it up.
		mfgActive = (m_featureStatusMfgActive || (paused && m_mfg.InputSize().cx > 0)) &&
			m_mfgMultiplier.load(std::memory_order_relaxed) >= 2 &&
			!m_mfgPresentFailed.load(std::memory_order_relaxed) && !m_mfg.Failed() && !MfgSourceMeetsDisplayRate();
		if (paused) m_featureStatusMfgActive = mfgActive;
		if (m_featureStatusUntil && now >= m_featureStatusUntil) return 0;
	}

	const bool srActive = m_iVPSuperRes == SUPERRES_DLSS_SR && m_dlssSR.Active();
	const auto processingRect = ProcessingRect();
	const bool upscale = m_srcRectWidth > 0 && m_srcRectHeight > 0 &&
		m_srcRectWidth < processingRect.Width() && m_srcRectHeight < processingRect.Height();
	const bool vsrActive = m_iVPSuperRes != SUPERRES_DLSS_SR &&
		m_VendorId == PCIV_NVIDIA && m_bVPScaling && m_bVPUseSuperRes && upscale;
	const auto* text = CMpcVideoRenderer::FeatureStatusText(srActive, vsrActive, mfgActive);
	if (!*text) return 0;

	constexpr LONG margin = 12, padX = 9, padY = 5;
	if (FAILED(m_Font3D.CreateFontBitmap(L"Consolas", m_StatsFontH, 0))) {
		return 0;
	}
	SIZE textSize{};
	m_Font3D.GetTextExtent(text, &textSize);
	const LONG width = textSize.cx + 2 * padX;
	const LONG height = textSize.cy + 2 * padY;
	if (width + 2 * margin > size.cx || height + 2 * margin > size.cy) {
		return 0;
	}

	const LONG x = margin;
	const LONG y = margin;
	CComPtr<ID3D11RenderTargetView> rtv;
	const HRESULT hr = m_pDevice->CreateRenderTargetView(target, nullptr, &rtv);
	if (FAILED(hr)) {
		return 0;
	}
	const RECT panel{x, y, x + width, y + height};
	if (SUCCEEDED(m_FeatureStatusBackground.Set(panel, size, D3DCOLOR_ARGB(170, 0, 0, 0)))) {
		m_FeatureStatusBackground.Draw(rtv, size);
	}
	const HRESULT drawn = m_Font3D.Draw2DText(rtv, size, x + padX, y + padY, D3DCOLOR_XRGB(255, 255, 255), text);
	if (!paused && SUCCEEDED(drawn)) {
		std::lock_guard guard(m_featureStatusMutex);
		if (!m_featureStatusUntil) {
			m_featureStatusUntil = now + 5000;
		}
	}
	return SUCCEEDED(drawn) ? height + margin : 0;
}

HRESULT CDX11VideoProcessor::DrawStats(ID3D11Texture2D* pRenderTarget, LONG featureOffset)
{
	if (m_windowRect.IsRectEmpty()) {
		return E_ABORT;
	}
	if (m_statsFeatureOffset != featureOffset) {
		m_statsFeatureOffset = featureOffset;
		CalcStatsParams();
	}

	std::wstring str;
	str.reserve(700);
	str.assign(m_strStatsHeader);
	str.append(m_strStatsDispInfo);
	str += std::format(L"\nGraph. Adapter: {}", m_strAdapterDescription);

	wchar_t frametype = (m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE) ? 'i' : 'p';
	const double sourceFps=m_pFilter->m_FrameStats.GetAverageFps();
	str += std::format(
		L"\nFrame rate    : {:7.3f}{},{:7.3f}",
		sourceFps,
		frametype,
		m_pFilter->m_DrawStats.GetAverageFps()
	);
	if (const auto corrected = m_pFilter->m_correctedSourcePts) {
		str += std::format(L"\nSource PTS    : {} corrected", corrected);
	}
	if (const auto backwards = m_pFilter->m_FrameStats.GetBackwardsTimestamps()) {
		str += std::format(L"\nSource PTS    : {} out of order", backwards);
	}

	str.append(m_strStatsInputFmt);
	if (m_Dovi.bValid && m_Dovi.bHasMMR) {
		str.append(L", MMR");
	}
	str.append(m_strStatsVProc);

	const bool projected = CanProjectJvr();
	const auto picture = PictureForPlayback(projected);
	const auto source = ProcessingSourceRect(projected);
	const UINT dstW = picture.Width();
	const UINT dstH = picture.Height();
	if (projected) {
		str += std::format(L"\nScaling       : {}x{} -> {}x{} (JVR 16:9)", source.Width(), source.Height(), dstW, dstH);
	} else if (m_iRotation) {
		str += std::format(L"\nScaling       : {}x{} r{}\u00B0> {}x{}", m_srcRectWidth, m_srcRectHeight, m_iRotation, dstW, dstH);
	} else {
		str += std::format(L"\nScaling       : {}x{} -> {}x{}", m_srcRectWidth, m_srcRectHeight, dstW, dstH);
	}
	if (UINT(source.Width()) != dstW || UINT(source.Height()) != dstH) {
		if (m_D3D11VP.IsReady() && m_bVPScaling && !m_bVPScalingUseShaders) {
			str.append(L" D3D11");
			if (m_bVPUseSuperRes && m_srcRectWidth < dstW && m_srcRectHeight < dstH) {
				str.append(L" SuperResolution*");
			}
		} else {
			str += L' ';
			if (m_strShaderX) {
				str.append(m_strShaderX);
				if (m_strShaderY && m_strShaderY != m_strShaderX) {
					str += L'/';
					str.append(m_strShaderY);
				}
			} else if (m_strShaderY) {
				str.append(m_strShaderY);
			}
		}
	}

	if (m_strCorrection || m_pPostScaleShaders.size() || m_bDitherUsed) {
		str.append(L"\nPostProcessing:");
		if (m_strCorrection) {
			str += std::format(L" {},", m_strCorrection);
		}
		if (m_pPostScaleShaders.size()) {
			str += std::format(L" shaders[{}],", m_pPostScaleShaders.size());
		}
		if (m_bDitherUsed) {
			str.append(L" dither");
		}
		str_trim_end(str, ',');
	}
	str.append(m_strStatsHDR);
	if ((m_bSdrMeasureActive || m_bHdrLocalMeasureActive) && m_fHdrMeasuredPeakNits > 0.0f) {
		// the SDR shader keeps the peak above the display's own, so show what it actually used
		const float usedNits = m_bSdrMeasureActive
			? std::clamp(m_fHdrSmoothedPeakNits, static_cast<float>(m_iSDRDisplayNits) + 1.0f, 10000.0f)
			: std::min(m_fHdrSmoothedPeakNits, 10000.0f);
		str += std::format(L"\nHDR measured  : frame peak {:.0f}, used {:.0f} nits", m_fHdrMeasuredPeakNits, usedNits);
	}
	str.append(m_strStatsPresent);

	str += std::format(L"\nFrames        : {:5}, skipped: {}/{}, failed: {}",
		m_pFilter->m_FrameStats.GetFrames(), m_pFilter->m_DrawStats.m_dropped, m_RenderStats.dropped2, m_RenderStats.failed);

	str += std::format(L"\nTimes(ms)     : Copy{:3}, Paint{:3}, Present{:3}",
		m_RenderStats.copyticks    * 1000 / GetPreciseTicksPerSecondI(),
		m_RenderStats.paintticks   * 1000 / GetPreciseTicksPerSecondI(),
		m_RenderStats.presentticks * 1000 / GetPreciseTicksPerSecondI());

	str += std::format(L"\nSync offset   : {:+3} ms", (m_RenderStats.syncoffset + 5000) / 10000);

#if SYNC_OFFSET_EX
	{
		const auto [so_min, so_max] = m_Syncs.MinMax();
		const auto [sod_min, sod_max] = m_SyncDevs.MinMax();
		str += std::format(L", range[{:+3.0f};{:+3.0f}], max change{:+3.0f}/{:+3.0f}",
			so_min / 10000.0f,
			so_max / 10000.0f,
			sod_min / 10000.0f,
			sod_max / 10000.0f);
	}
#endif
#if TEST_TICKS
	str += std::format(L"\n1:{:6.3f}, 2:{:6.3f}, 3:{:6.3f}, 4:{:6.3f}, 5:{:6.3f}, 6:{:6.3f} ms",
		m_RenderStats.t1 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t2 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t3 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t4 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t5 * 1000 / GetPreciseTicksPerSecond(),
		m_RenderStats.t6 * 1000 / GetPreciseTicksPerSecond());
#endif

	std::wstring mfgStats;
	if (m_mfgMultiplier) {
		if (MfgSourceMeetsDisplayRate()) {
			mfgStats = L"\nMFG      : Bypassed (source meets display refresh)";
		} else if (m_mfgPresentFailed) {
			mfgStats = L"\nMFG      : Presenter failed";
		} else if (m_mfg.Failed()) {
			mfgStats = L"\nMFG      : Unavailable";
		} else if (m_pFilter->m_filterState == State_Paused) {
			mfgStats = L"\nMFG      : Paused";
		} else {
			const auto now=int64_t(GetPreciseTick());
			const auto real=m_mfgRealPresents.load(std::memory_order_relaxed);
			const auto generated=m_mfgGeneratedPresents.load(std::memory_order_relaxed);
			const auto fallback=m_mfgFallbackPresents.load(std::memory_order_relaxed);
			const auto skipped=m_mfgSkipped.load(std::memory_order_relaxed);
			double outputFps = 0.0, skippedPerSec = 0.0;
			{
				std::lock_guard guard(m_mfgRateLock);
				const double frequency=double(GetPreciseTicksPerSecond());
				if (!m_mfgRateTick || now<m_mfgRateTick) {
					m_mfgRateTick=now;
					m_mfgRateReal=real;
					m_mfgRateGenerated=generated;
					m_mfgRateFallback=fallback;
					m_mfgRateSkipped=skipped;
					m_mfgRateVideoCount=m_mfgVideoCount.load(std::memory_order_relaxed);
					m_mfgRateVideoTicks=m_mfgVideoTicks.load(std::memory_order_relaxed);
					m_mfgRateGenerateCount=m_mfgGenerateCount.load(std::memory_order_relaxed);
					m_mfgRateGenerateTicks=m_mfgGenerateTicks.load(std::memory_order_relaxed);
					m_mfgRateComposeCount=m_mfgComposeCount.load(std::memory_order_relaxed);
					m_mfgRateComposeTicks=m_mfgComposeTicks.load(std::memory_order_relaxed);
					m_mfgRateLockRetries=m_mfgLockRetries.load(std::memory_order_relaxed);
					m_mfgActualFps=m_mfgGeneratedFps=m_mfgSkippedPerSec=m_mfgFallbackFps=0.0;
					m_mfgVideoWallMs=m_mfgGenerateWallMs=m_mfgComposeWallMs=0.0;
				}
				else if (frequency>0 && double(now-m_mfgRateTick)>=frequency) {
					const double seconds=double(now-m_mfgRateTick)/frequency;
					const auto realDelta=real>=m_mfgRateReal?real-m_mfgRateReal:0;
					const auto genDelta=generated>=m_mfgRateGenerated?generated-m_mfgRateGenerated:0;
					const auto fallbackDelta=fallback>=m_mfgRateFallback?fallback-m_mfgRateFallback:0;
					const auto skipDelta=skipped>=m_mfgRateSkipped?skipped-m_mfgRateSkipped:0;
					m_mfgActualFps=double(realDelta+genDelta+fallbackDelta)/seconds;
					if (mfg::SourceCadenceTrace().Enabled()) {
						// Window counts are stored in serial (source), phase (generated),
						// and effective_multiplier (fallback). The unit of duration is QPC.
						mfg::SourceCadenceTrace().Record("mfg_stats_window", now, -1, -1,
							realDelta, unsigned(genDelta), unsigned(fallbackDelta), 0, 0, false,
							m_mfgLostGenerated, S_OK, now - m_mfgRateTick);
					}
					m_mfgGeneratedFps=double(genDelta)/seconds;
					m_mfgFallbackFps=double(fallbackDelta)/seconds;
					m_mfgSkippedPerSec=double(skipDelta)/seconds;
					const auto videoCount=m_mfgVideoCount.load(std::memory_order_relaxed);
					const auto videoTicks=m_mfgVideoTicks.load(std::memory_order_relaxed);
					const auto videoCallCount=videoCount>=m_mfgRateVideoCount?videoCount-m_mfgRateVideoCount:0;
					const auto videoWallTicks=videoTicks>=m_mfgRateVideoTicks?videoTicks-m_mfgRateVideoTicks:0;
					m_mfgVideoWallMs=videoCallCount?1000.0*double(videoWallTicks)/frequency/double(videoCallCount):0.0;
					m_mfgRateVideoCount=videoCount;m_mfgRateVideoTicks=videoTicks;
					const auto generateCount=m_mfgGenerateCount.load(std::memory_order_relaxed);
					const auto generateTicks=m_mfgGenerateTicks.load(std::memory_order_relaxed);
					const auto composeCount=m_mfgComposeCount.load(std::memory_order_relaxed);
					const auto composeTicks=m_mfgComposeTicks.load(std::memory_order_relaxed);
					const auto workCount=generateCount>=m_mfgRateGenerateCount?generateCount-m_mfgRateGenerateCount:0;
					const auto workTicks=generateTicks>=m_mfgRateGenerateTicks?generateTicks-m_mfgRateGenerateTicks:0;
					const auto presentCount=composeCount>=m_mfgRateComposeCount?composeCount-m_mfgRateComposeCount:0;
					const auto presentTicks=composeTicks>=m_mfgRateComposeTicks?composeTicks-m_mfgRateComposeTicks:0;
					m_mfgGenerateWallMs=workCount?1000.0*double(workTicks)/frequency/double(workCount):0.0;
					m_mfgComposeWallMs=presentCount?1000.0*double(presentTicks)/frequency/double(presentCount):0.0;
					m_mfgRateGenerateCount=generateCount;m_mfgRateGenerateTicks=generateTicks;
					m_mfgRateComposeCount=composeCount;m_mfgRateComposeTicks=composeTicks;
					const auto lockRetries=m_mfgLockRetries.load(std::memory_order_relaxed);
					m_mfgLockRetriesPerSec=lockRetries>=m_mfgRateLockRetries?
						double(lockRetries-m_mfgRateLockRetries)/seconds:0.0;
					m_mfgRateLockRetries=lockRetries;
					// Read DXGI's last display statistics once per rate window, never in
					// the critical Present path. Unsupported/disjoint windows show n/a.
					DXGI_FRAME_STATISTICS dxgi{};
					const bool supported=m_pDXGISwapChain1 &&
						(m_bExclusiveScreen || m_UsedSwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD ||
						 m_UsedSwapEffect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
					const HRESULT statsHr=supported?m_pDXGISwapChain1->GetFrameStatistics(&dxgi):E_FAIL;
					if(SUCCEEDED(statsHr)) {
						if(m_mfgDxgiHasBaseline && dxgi.PresentCount>=m_mfgDxgiLastPresentCount) {
							const double rate=double(dxgi.PresentCount-m_mfgDxgiLastPresentCount)/seconds;
							const double displayHz=m_dRefreshPeriodMs>0.0?1000.0/m_dRefreshPeriodMs:0.0;
							// DXGI stats can be discontinuous on mode changes / some display paths.
							// Never report an impossible scan-out rate as a measurement.
							m_mfgDxgiAvailable=displayHz>0.0 && rate<=displayHz*1.2+1.0;
							if(m_mfgDxgiAvailable) m_mfgDxgiDisplayFps=rate;
						} else m_mfgDxgiAvailable=false;
						m_mfgDxgiLastPresentCount=dxgi.PresentCount;
						m_mfgDxgiHasBaseline=true;
					} else {m_mfgDxgiHasBaseline=false;m_mfgDxgiAvailable=false;}
					m_mfgRateTick=now;
					m_mfgRateReal=real;
					m_mfgRateGenerated=generated;
					m_mfgRateFallback=fallback;
					m_mfgRateSkipped=skipped;
				}
				outputFps=m_mfgActualFps;
				skippedPerSec=m_mfgSkippedPerSec;
			}
			const auto inputSize = m_mfg.InputSize();
			const auto outputSize = m_mfg.GeneratedSize();
			if (inputSize.cx && sourceFps > 0.0 && outputFps > 0.0) {
				// This rate counts successful Presents and may differ from scan-out.
				const auto fps = [](double value) {
					auto text = std::format(L"{:.2f}", value);
					while (text.back() == L'0') text.pop_back();
					if (text.back() == L'.') text.pop_back();
					return text;
				};
				mfgStats = std::format(L"\nMFG      : {}x{} {}fps -> {}x{} {}fps",
					inputSize.cx, inputSize.cy, fps(sourceFps), outputSize.cx, outputSize.cy, fps(outputFps));
				if (skippedPerSec >= 0.1) mfgStats += std::format(L" | {:.1f} skipped/s", skippedPerSec);
			} else {
				mfgStats = L"\nMFG      : Waiting for frames";
			}
		}
	}

	std::wstring srStats;
	if (m_iVPSuperRes == SUPERRES_DLSS_SR) {
		const auto stats = m_dlssSR.GetStats();
		if (m_dlssSR.Preparing()) {
			srStats = L"\nDLSS SR  : Preparing";
		} else if (!stats.inputWidth) {
			srStats = L"\nDLSS SR  : Inactive";
			if (m_dlssSRInputStatus) srStats += std::format(L" ({})", m_dlssSRInputStatus);
		} else if (stats.inputWidth == stats.outputWidth && stats.inputHeight == stats.outputHeight) {
			srStats = std::format(L"\nDLAA     : {}x{}", stats.inputWidth, stats.inputHeight);
		} else {
			srStats = std::format(L"\nDLSS SR  : {}x{} -> {}x{}",
				stats.inputWidth, stats.inputHeight, stats.outputWidth, stats.outputHeight);
		}
	}

	std::wstring depthStats;
	if (UsesZipDepthSr() || UsesZipDepthMfg()) {
		const auto& depth = m_zipDepth.GetCounters();
		const auto* precision = m_zipDepth.ModelPrecision();
		if (depth.maps && *precision) {
			depthStats = std::format(L"\nZipDepth : {}, {:.1f} maps/s", precision, depth.mapsPerSecond);
			if (depth.lateMaps) depthStats += std::format(L", {} late", depth.lateMaps);
			const auto now = GetTickCount64();
			const bool srUsed = UsesZipDepthSr() &&
				depth.srEvaluations && now - depth.lastSrUseTick <= 1000;
			const bool mfgUsed = UsesZipDepthMfg() && depth.mfgSubmissions && now - depth.lastMfgUseTick <= 1000;
			if (srUsed || mfgUsed) {
				depthStats += srUsed && mfgUsed ? L" | SR + MFG" : srUsed ? L" | SR" : L" | MFG";
			}
			if (m_zipDepth.CoolingDown(now)) depthStats += L" | paused (late)";
		} else {
			depthStats = std::format(L"\nZipDepth : {}", m_zipDepth.Status());
		}
	}

	std::wstring bufferStats;
	CRefTime bufferStream;
	if ((m_completedLast || CompletedFramesPending()) && SUCCEEDED(m_pFilter->StreamTime(bufferStream))) {
		const auto time = GetCompletedBufferTime(bufferStream);
		bufferStats = std::format(L"\nVideo buffer : {} ms ready / {} ms queued", time.ready / 10000, time.queued / 10000);
		if (time.nonIncreasing) bufferStats += std::format(L" | {} due out of order", time.nonIncreasing);
	}
	const bool hasEditionStats = m_svdLut.strength || !srStats.empty() || !mfgStats.empty() ||
		!depthStats.empty() || !bufferStats.empty();
	const auto separator = str.size() + 1;
	if (hasEditionStats) {
		str += L'\n'; // Reserve a row before the layout computes the text width.
		if (!srStats.empty() && m_dlssSR.Active() && m_dlssSRAfterMfg) {
			str.append(mfgStats).append(srStats);
		} else {
			str.append(srStats).append(mfgStats);
		}
		str.append(depthStats);
		if (m_svdLut.strength) str += std::format(L"\nSVDLUT   : {} ({}%)", SvdLutStatus(), m_svdLut.strength);
		str.append(bufferStats);
	}
	if (UpdateStatsLayout(str)) {
		UpdateStatsByWindow();
	}
	if (hasEditionStats) {
		SIZE dash{};
		m_Font3D.GetTextExtent(L"-", &dash);
		const LONG right = std::min<LONG>(m_StatsRect.right - 3, m_windowRect.Width());
		const LONG width = std::max<LONG>(0, right - m_StatsTextPoint.x);
		str.insert(separator, dash.cx > 0 ? width / dash.cx : 0, L'-');
	}

	ID3D11RenderTargetView* pRenderTargetView = nullptr;
	HRESULT hr = m_pDevice->CreateRenderTargetView(pRenderTarget, nullptr, &pRenderTargetView);
	if (S_OK == hr) {
		SIZE rtSize = m_windowRect.Size();

		m_StatsBackground.Draw(pRenderTargetView, rtSize);

		hr = m_Font3D.Draw2DText(pRenderTargetView, rtSize, m_StatsTextPoint.x,
			m_StatsTextPoint.y + m_statsFeatureOffset, m_dwStatsTextColor, str.c_str());
		if (--m_StatsMarkerX < m_StatsRect.left || m_StatsMarkerX > m_StatsRect.right) {
			m_StatsMarkerX = m_StatsRect.right;
		}
		m_Rect3D.Set({ m_StatsMarkerX, m_StatsRect.bottom - kStatsMarkerH - 1, m_StatsMarkerX + 5, m_StatsRect.bottom - 1 }, rtSize, D3DCOLOR_XRGB(128, 255, 128));
		m_Rect3D.Draw(pRenderTargetView, rtSize);


		if (CheckGraphPlacement()) {
			m_Underlay.Draw(pRenderTargetView, rtSize);

			m_Lines.Draw();

			m_SyncLine.ClearPoints(rtSize);
			m_SyncLine.AddGFPoints(m_GraphRect.left, m_Xstep, m_Yaxis, m_Yscale,
				m_Syncs.Data(), m_Syncs.OldestIndex(), m_Syncs.Size(), D3DCOLOR_XRGB(100, 200, 100));
			m_SyncLine.UpdateVertexBuffer();
			m_SyncLine.Draw();
		}
		pRenderTargetView->Release();
	}

	return hr;
}

// IMFVideoProcessor

STDMETHODIMP CDX11VideoProcessor::SetProcAmpValues(DWORD dwFlags, DXVA2_ProcAmpValues *pValues)
{
	CheckPointer(pValues, E_POINTER);
	if (m_srcParams.cformat == CF_NONE) {
		return MF_E_TRANSFORM_TYPE_NOT_SET;
	}

	if (dwFlags & DXVA2_ProcAmp_Brightness) {
		m_DXVA2ProcAmpValues.Brightness.ll = std::clamp(pValues->Brightness.ll, m_DXVA2ProcAmpRanges[0].MinValue.ll, m_DXVA2ProcAmpRanges[0].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Contrast) {
		m_DXVA2ProcAmpValues.Contrast.ll = std::clamp(pValues->Contrast.ll, m_DXVA2ProcAmpRanges[1].MinValue.ll, m_DXVA2ProcAmpRanges[1].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Hue) {
		m_DXVA2ProcAmpValues.Hue.ll = std::clamp(pValues->Hue.ll, m_DXVA2ProcAmpRanges[2].MinValue.ll, m_DXVA2ProcAmpRanges[2].MaxValue.ll);
	}
	if (dwFlags & DXVA2_ProcAmp_Saturation) {
		m_DXVA2ProcAmpValues.Saturation.ll = std::clamp(pValues->Saturation.ll, m_DXVA2ProcAmpRanges[3].MinValue.ll, m_DXVA2ProcAmpRanges[3].MaxValue.ll);
	}

	if (dwFlags & DXVA2_ProcAmp_Mask) {
		CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

		m_D3D11VP.SetProcAmpValues(&m_DXVA2ProcAmpValues);

		if (!m_D3D11VP.IsReady()) {
			SetShaderConvertColorParams();
		}
	}

	return S_OK;
}

// IMFVideoMixerBitmap

STDMETHODIMP CDX11VideoProcessor::SetAlphaBitmap(const MFVideoAlphaBitmap *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

	HRESULT hr = S_FALSE;

	if (pBmpParms->GetBitmapFromDC && pBmpParms->bitmap.hdc) {
		HBITMAP hBitmap = (HBITMAP)GetCurrentObject(pBmpParms->bitmap.hdc, OBJ_BITMAP);
		if (!hBitmap) {
			return E_INVALIDARG;
		}
		DIBSECTION info = {0};
		if (!::GetObjectW(hBitmap, sizeof(DIBSECTION), &info)) {
			return E_INVALIDARG;
		}
		BITMAP& bm = info.dsBm;
		if (!bm.bmWidth || !bm.bmHeight || bm.bmBitsPixel != 32 || !bm.bmBits) {
			return E_INVALIDARG;
		}

		hr = m_TexAlphaBitmap.CheckCreate(m_pDevice, DXGI_FORMAT_B8G8R8A8_UNORM, bm.bmWidth, bm.bmHeight, Tex2D_DefaultShader);
		DLogIf(FAILED(hr), L"CDX11VideoProcessor::SetAlphaBitmap() : CheckCreate() failed with error {}", HR2Str(hr));
		if (S_OK == hr) {
			m_pDeviceContext->UpdateSubresource(m_TexAlphaBitmap.pTexture, 0, nullptr, bm.bmBits, bm.bmWidthBytes, 0);
		}
	} else {
		return E_INVALIDARG;
	}

	m_bAlphaBitmapEnable = SUCCEEDED(hr) && m_TexAlphaBitmap.pShaderResource;

	if (m_bAlphaBitmapEnable) {
		m_AlphaBitmapRectSrc = { 0, 0, (LONG)m_TexAlphaBitmap.desc.Width, (LONG)m_TexAlphaBitmap.desc.Height };
		m_AlphaBitmapNRectDest = { 0, 0, 1, 1 };

		m_pAlphaBitmapVertex.Release();

		hr = UpdateAlphaBitmapParameters(&pBmpParms->params);
	}

	return hr;
}

STDMETHODIMP CDX11VideoProcessor::UpdateAlphaBitmapParameters(const MFVideoAlphaBitmapParams *pBmpParms)
{
	CheckPointer(pBmpParms, E_POINTER);
	CAutoLock cRendererLock(&m_pFilter->m_RendererLock);

	if (m_bAlphaBitmapEnable) {
		if (pBmpParms->dwFlags & MFVideoAlphaBitmap_SrcRect) {
			m_AlphaBitmapRectSrc = pBmpParms->rcSrc;
			m_pAlphaBitmapVertex.Release();
		}
		if (!m_pAlphaBitmapVertex) {
			HRESULT hr = CreateVertexBuffer(m_pDevice, &m_pAlphaBitmapVertex, m_TexAlphaBitmap.desc.Width, m_TexAlphaBitmap.desc.Height, m_AlphaBitmapRectSrc, 0, false);
			if (FAILED(hr)) {
				m_bAlphaBitmapEnable = false;
				return hr;
			}
		}
		if (pBmpParms->dwFlags & MFVideoAlphaBitmap_DestRect) {
			m_AlphaBitmapNRectDest = pBmpParms->nrcDest;
		}
		DWORD validFlags = MFVideoAlphaBitmap_SrcRect|MFVideoAlphaBitmap_DestRect;

		return ((pBmpParms->dwFlags & validFlags) == validFlags) ? S_OK : S_FALSE;
	} else {
		return MF_E_NOT_INITIALIZED;
	}
}

void CDX11VideoProcessor::SetCallbackDevice()
{
	if (!m_bCallbackDeviceIsSet && m_pDevice && m_pFilter->m_pSub11CallBack) {
		m_bCallbackDeviceIsSet = SUCCEEDED(m_pFilter->m_pSub11CallBack->SetDevice11(m_pDevice));
	}
}

void CDX11VideoProcessor::UpdateSubPic()
{
	ASSERT(m_pDevice);

	if (m_pFilter->m_pSubPicProvider) {
		if (m_pSubPicAllocator) {
			m_pSubPicAllocator->ChangeDevice(m_pDevice);
		}

		if (m_pFilter->m_pSubPicQueue) {
			m_pFilter->m_pSubPicQueue->Invalidate();
			m_pFilter->m_pSubPicQueue->SetSubPicProvider(m_pFilter->m_pSubPicProvider);
		}
	}
}

void CDX11VideoProcessor::SwitchFullScreen(bool set)
{
	m_bFullScreen = set;

	if (HandleHDRToggle()) {
		InitMediaType(&m_pFilter->m_inputMT);
		InitSwapChain(false);
	}
}

void CDX11VideoProcessor::SetPGSSubtitles(bool bPGSSubtitles) {
	if (bPGSSubtitles != m_bPGSSubtitles) {
		m_bPGSSubtitles = bPGSSubtitles;

		CAutoLock cRendererLock(&m_pFilter->m_RendererLock);
		UpdateBitmapShader();
	}
}
