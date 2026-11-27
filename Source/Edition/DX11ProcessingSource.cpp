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

// Resolves only Edition sources. nullptr leaves native VP/color conversion in charge.
// Returned surfaces are borrowed; this call does not transfer GPU ownership.
const Tex2D_t* CDX11VideoProcessor::SelectEditionProcessingSource(const CRect& srcRect,
	const CRect& dstRect, bool second, bool allowDlssSR, REFERENCE_TIME timestamp,
	bool* overlayInputHandled, const Tex2D_t* preparedSource, CRect& processingSourceRect)
{
	CRect& rSrc = processingSourceRect;
	const bool srWasActive = m_dlssSR.Active();
	auto* srOutput = allowDlssSR ? TryDlssSR(srcRect, dstRect, second, timestamp, overlayInputHandled, preparedSource) : nullptr;
	if (allowDlssSR && !srOutput && srWasActive) {
		m_dlssSR.ResetHistory();
	}
	if (allowDlssSR && srWasActive != bool(srOutput) && m_mfgMultiplier && !MfgSourceMeetsDisplayRate()) {
		if (m_pFilter->m_Sets.bWaitForExtendedStartup) {
            // Native MFG may have refused before this ordinary SR/VSR
            // fallback was rendered. Its retired originals must keep their
            // paced owner until the final current picture can be enqueued.
			if (!m_mfgLegacyFallbackActive || m_completingSample) StopMfg(false);
		} else {
			// Completed originals still belong to the previous visual route.
			// Keep their deadlines and restart interpolation at this boundary.
			ResetMfgFrameAnalysis();
			m_mfgRepeatTiming.ClearEvidence();
		}
		m_mfg.ResetHistory();
	}
	if (srOutput) {
		rSrc.SetRect(0, 0, srOutput->desc.Width, srOutput->desc.Height);
        return srOutput;
	}
    // The SR branch above already composes into its native-size FP16 input.
    // When SR is absent/bypassed, keep the same original-first rule. Calls
    // used solely for native MFG conversion have INVALID_TIME: QueueMfg owns
    // their colour stage, and never re-enters this branch for generated frames.
    if(timestamp>=0 && (PerceptualEnabled() || OptionalColorSupported()) && SourceToneEligible() &&
        !m_bInterlaced && !m_iRotation && !m_bFlip && !DepthOverlayEnabled() &&
        srcRect.Width()>1 && srcRect.Height()>1) {
        auto& a=m_adaptiveTone;
        const CRect native(0,0,srcRect.Width(),srcRect.Height());
        const bool vsrFirst=!preparedSource && m_D3D11VP.IsReady() && m_bVPUseSuperRes;
        const Tex2D_t* raw=preparedSource;
        if(vsrFirst) {
            // The stock VP combines YUV->RGB and VSR. Split it only when its
            // NVIDIA RGB extension can preserve the selected VSR-before-MFG
            // route. Non-default ProcAmp is left on the original VP path.
            const bool neutralProcAmp=m_DXVA2ProcAmpValues.Brightness.ll==0 &&
                m_DXVA2ProcAmpValues.Hue.ll==0 && m_DXVA2ProcAmpValues.Contrast.ll==65536 &&
                m_DXVA2ProcAmpValues.Saturation.ll==65536;
            const bool supported=m_VendorId==PCIV_NVIDIA && m_srcParams.CDepth==8 && neutralProcAmp &&
                m_D3D11OutputFmt==DXGI_FORMAT_B8G8R8A8_UNORM && !m_Dovi.bValid &&
                m_SampleFormat==D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE &&
                dstRect.Width()>native.Width() && dstRect.Height()>native.Height();
            if(!supported) return preparedSource;
            const std::array<UINT,7> key{UINT(m_srcDXGIFormat),UINT(m_srcWidth),UINT(m_srcHeight),
                UINT(native.Width()),UINT(native.Height()),UINT(m_iVPSuperRes),UINT(m_srcExFmt.value)};
            if(a.sourceVsrKey!=key) {
                a.sourceCscVP.ReleaseVideoDevice();
                a.sourceVsrVP.ReleaseVideoDevice();
                a.sourceVsrKey=key;
                a.sourceVsrReady=false;
                DXGI_FORMAT preFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
                DXGI_FORMAT postFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
                DXVA2_ExtendedFormat rgb{};
                a.sourceVsrReady=SUCCEEDED(a.sourceCscVP.InitVideoDevice(m_pDevice,m_pDeviceContext,m_VendorId)) &&
                    SUCCEEDED(a.sourceCscVP.InitVideoProcessor(m_srcDXGIFormat,m_srcWidth,m_srcHeight,m_srcExFmt,DEINT_Disable,false,preFormat)) &&
                    preFormat==DXGI_FORMAT_B8G8R8A8_UNORM &&
                    SUCCEEDED(a.sourceVsrVP.InitVideoDevice(m_pDevice,m_pDeviceContext,m_VendorId)) &&
                    SUCCEEDED(a.sourceVsrVP.InitVideoProcessor(preFormat,native.Width(),native.Height(),rgb,DEINT_Disable,false,postFormat)) &&
                    postFormat==preFormat && a.sourceVsrVP.SetSuperRes(m_iVPSuperRes)==S_OK;
                DLog(L"VE source -> RTX VSR RGB bridge: {}",a.sourceVsrReady?L"accepted by driver":L"unavailable; native VSR preserved");
            }
            if(!a.sourceVsrReady) return preparedSource;
            const auto held=m_D3D11VP.HoldFrame();
            if(!held.pTexture || FAILED(a.sourceRaw.CheckCreate(m_pDevice,DXGI_FORMAT_B8G8R8A8_UNORM,
                    native.Width(),native.Height(),Tex2D_DefaultShaderRTarget)) ||
                FAILED(a.sourceCscVP.ProcessExternal(held.pTexture,held.ArraySlice,a.sourceRaw.pTexture,srcRect,native))) {
                a.sourceVsrReady=false;
                return preparedSource;
            }
            raw=&a.sourceRaw;
        } else if(!raw && m_D3D11VP.IsReady()) {
            // Keep the selected stock hardware scaler when no AI upscale is active.
            if(m_bVPScaling && dstRect.Size()!=native.Size()) return preparedSource;
            raw=ReuseEarlyOriginal(srcRect,dstRect,timestamp,second);
            if(!raw && SUCCEEDED(a.sourceRaw.CheckCreate(m_pDevice,m_D3D11OutputFmt,
                native.Width(),native.Height(),Tex2D_DefaultShaderRTarget)) &&
                SUCCEEDED(D3D11VPPass(a.sourceRaw.pTexture,srcRect,native,second))) raw=&a.sourceRaw;
        } else if(!raw && m_PSConvColorData.bEnable) {
            if(m_TexConvertOutput.desc.Width==UINT(native.Width()) && m_TexConvertOutput.desc.Height==UINT(native.Height()) &&
                SUCCEEDED(ConvertColorPass(m_TexConvertOutput.pTexture))) raw=&m_TexConvertOutput;
        } else if(!raw && srcRect==CRect(0,0,m_TexSrcVideo.desc.Width,m_TexSrcVideo.desc.Height)) {
            raw=&m_TexSrcVideo;
        }
        if(raw && raw->pTexture && raw->pShaderResource && raw->desc.Width==UINT(native.Width()) && raw->desc.Height==UINT(native.Height())) {
            const auto fmt=raw->desc.Format;
            if(fmt!=DXGI_FORMAT_B8G8R8A8_UNORM && fmt!=DXGI_FORMAT_B8G8R8X8_UNORM &&
                fmt!=DXGI_FORMAT_R8G8B8A8_UNORM && fmt!=DXGI_FORMAT_R10G10B10A2_UNORM) return preparedSource;
            // PrepareSourceTone returns the original if shader/model work is
            // still pending. Such a frame remains neutral, not post-graded.
            auto* corrected=PrepareSourceTone(*raw,native,timestamp);
            if(corrected==a.sourceOutput.pTexture.p && corrected!=raw->pTexture.p) raw=&a.sourceOutput;
            if(vsrFirst) {
                const CRect output(0,0,dstRect.Width(),dstRect.Height());
                m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
                ID3D11ShaderResourceView* empty=nullptr;
                m_pDeviceContext->PSSetShaderResources(0,1,&empty);
                if(!corrected || FAILED(a.sourceVsrOutput.CheckCreate(m_pDevice,DXGI_FORMAT_B8G8R8A8_UNORM,
                        output.Width(),output.Height(),Tex2D_DefaultShaderRTarget)) ||
                    FAILED(a.sourceVsrVP.ProcessExternal(corrected,0,a.sourceVsrOutput.pTexture,native,output))) {
                    a.sourceVsrReady=false;
                    DLog(L"VE source -> RTX VSR RGB bridge failed; preserving native VSR");
                    return preparedSource;
                }
                rSrc=output;
                RecordEditionVsr(srcRect,dstRect,false);
                raw=&a.sourceVsrOutput;
            } else rSrc=native;
            a.sourceToneBeforeUpscale=true;
            m_bVPScalingUseShaders=m_D3D11VP.IsReady();
            return raw;
        }
    }
    if(preparedSource) m_bVPScalingUseShaders=m_D3D11VP.IsReady();
    return preparedSource;
}
