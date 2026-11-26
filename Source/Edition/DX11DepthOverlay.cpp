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

struct DEPTH3D_VERTEX {
	DirectX::XMFLOAT2 Pos;
	DirectX::XMFLOAT2 TexCoord;
	DirectX::XMFLOAT2 DepthCoord;
};


bool CDX11VideoProcessor::DepthOverlayEnabled() const
{
	return m_showDepthOverlay && (UsesDepthSr() || UsesDepthMfg()) && !m_depthOverlayShaderFailed &&
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
	m_depth.Update(m_pDevice, m_pDeviceContext, source, picture, modelSize, timestamp);
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
		(!UsesDepthSr() && !UsesDepthMfg())) return &frame.image;

	m_depth.PollCompleted();
	const auto estimate = m_depth.ForFrame(frame.pts);
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
		estimate.rotated ? 1.0f : 0.0f, kDepth3DZStrength, CDepth::NormalizedDepthSpan, kDepth3DCameraDistance};
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
	m_depth.PollCompleted();
	const auto estimate = m_depth.ForFrame(timestamp);
	if (!estimate.view) return;
	const auto tensor = m_depth.TensorSize();
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
    // Display-only false color: far purple/blue, middle cyan/green,
    // near yellow/orange/red. No change to the depth texture or NGX input.
    const float z = saturate(0.5 + (0.5 - depth) / DepthSpan);
    float3 color;
    if (z < 0.20) color = lerp(float3(0.17, 0.075, 0.21), float3(0.24, 0.20, 0.48), z / 0.20);
    else if (z < 0.38) color = lerp(float3(0.24, 0.20, 0.48), float3(0.04, 0.78, 0.99), (z - 0.20) / 0.18);
    else if (z < 0.53) color = lerp(float3(0.04, 0.78, 0.99), float3(0.18, 0.88, 0.50), (z - 0.38) / 0.15);
    else if (z < 0.68) color = lerp(float3(0.18, 0.88, 0.50), float3(0.99, 0.87, 0.13), (z - 0.53) / 0.15);
    else if (z < 0.85) color = lerp(float3(0.99, 0.87, 0.13), float3(1.00, 0.36, 0.015), (z - 0.68) / 0.17);
    else color = lerp(float3(1.00, 0.36, 0.015), float3(0.76, 0.075, 0.015), (z - 0.85) / 0.15);
    return float4(saturate(color), 1.0);
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
		inverted ? CDepth::OutputDepthSpan : CDepth::NormalizedDepthSpan};
	m_pDeviceContext->UpdateSubresource(m_pDepthOverlayConstants, 0, nullptr, settings, 0, 0);
	D3D11_VIEWPORT viewport{FLOAT(picture.left), FLOAT(picture.top),
		FLOAT(picture.Width()), FLOAT(picture.Height()), 0.0f, 1.0f};
	m_pDeviceContext->RSSetState(nullptr);
	const float opacity = 1.0f;
	const FLOAT blendFactor[]{opacity, opacity, opacity, 1.0f};
	TextureBlt11(m_pDeviceContext, targetView, viewport, m_pVSimpleInputLayout, m_pVS_Simple,
		m_pPS_DepthOverlay, depth, m_pSamplerLinear, m_pDepthOverlayConstants, m_pVertexBuffer,
		m_pDepthOverlayBlendState, blendFactor);
	m_pDeviceContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
}

