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
#include "VideoRenderer.h"

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
	if (UsesDepthSr()) m_depth.Prepare(m_pDevice);
	return m_dlssSR.Prepare(m_pDevice, srcRect.Width(), srcRect.Height(),
		dstRect.Width(), dstRect.Height(), !m_pFilter->m_Sets.bWaitForExtendedStartup);
}

Tex2D_t* CDX11VideoProcessor::EvaluateDlssSR(const Tex2D_t& source, Tex2D_t& input, const CRect& sourceRect,
                                             REFERENCE_TIME timestamp, float frameTimeMs, bool* overlayInputHandled, bool depthPrepared)
{
    // [FA-03B | 2026-11-25 15:09 +08:00] Only the original-frame SR route.
    // Input is the existing native-size FP16 NGX surface. Never grade a
    // generated frame, or change the combined VE path until its lookahead moves.
    const bool sourceFrame = !m_dlssSRAfterMfg;
    const CRect native(0,0,source.desc.Width,source.desc.Height);
    const bool sourceTone = sourceFrame && timestamp>=0 && SourceToneEligible() &&
        sourceRect==native && input.desc.Width==source.desc.Width && input.desc.Height==source.desc.Height &&
        input.desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT && input.pTexture &&
        source.pTexture && source.pShaderResource && !DepthOverlayEnabled();
    auto& tone=m_adaptiveTone;
    if(sourceFrame) {
        tone.sourceToneBeforeUpscale=false; // raw fallback must not inherit a prior result
        if(tone.srSourceToneRoute && *tone.srSourceToneRoute!=sourceTone) m_dlssSR.ResetHistory();
        tone.srSourceToneRoute=sourceTone;
    }
    const bool toneDrawn=sourceTone && PrepareSourceTone(source,sourceRect,timestamp,input.pTexture.p)==input.pTexture.p;
	if (!toneDrawn && FAILED(TextureCopyRect(source, input.pTexture, sourceRect, sourceRect, m_pPS_Simple, nullptr, 0, false))) {
		m_dlssSRInputStatus = L"source conversion failed";
		m_dlssSR.ResetHistory();
		return nullptr;
	}
	m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11ShaderResourceView* empty = nullptr;
	m_pDeviceContext->PSSetShaderResources(0, 1, &empty);
	const bool overlay = sourceFrame && overlayInputHandled && !m_mfgMultiplier &&
		!depthPrepared && BlendSourceDepthOverlay(input.pTexture, source.pShaderResource, sourceRect, timestamp);
    // Keep both depth consumers and SR scene detection on ungraded source.
    // A projected source has already submitted raw depth in RenderEditionVideo.
    if(sourceTone && !depthPrepared && (UsesDepthSr() || UsesDepthMfg()))
        m_depth.Update(m_pDevice,m_pDeviceContext,source.pShaderResource,sourceRect,sourceRect.Size(),timestamp);
	ID3D11ShaderResourceView* cleanSource = depthPrepared || overlay || sourceTone ? source.pShaderResource.p : nullptr;
	auto* output = m_dlssSR.Evaluate(timestamp, frameTimeMs, UsesDepthSr() ? &m_depth : nullptr,
		sourceFrame, cleanSource);
    // A failed evaluation leaves normal raw fallback in charge. Only a
    // returned SR output carries the upstream decision into MFG/Presenter.
    if(sourceFrame) tone.sourceToneBeforeUpscale=output && sourceTone;
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

