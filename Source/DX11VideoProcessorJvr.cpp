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
#include "Helper.h"
#include "DX11VideoProcessor.h"
#include "Shaders.h"

// Edition JVR implementation. Playback integration remains in the renderer.

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
	if (SUCCEEDED(result)) m_featureProjectionReady = true;
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

