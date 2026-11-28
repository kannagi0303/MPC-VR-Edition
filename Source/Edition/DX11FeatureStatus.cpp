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
#include "Mfg/DisplayCadence.h"

void CDX11VideoProcessor::ResetFeatureStatus()
{
    std::lock_guard guard(m_featureStatusMutex);
    m_featureNotice.Reset();
    m_featureProjectionReady = false;
    m_featureStatusLastPerceptual = m_perceptual.draws;
}

LONG CDX11VideoProcessor::DrawFeatureStatus(ID3D11Texture2D* target)
{
    if (!target || !m_pDevice || !m_showEditionOsd.load(std::memory_order_relaxed) ||
        m_pFilter->m_filterState != State_Running || m_pFilter->m_bFlushing) return 0;

    using namespace edition::osd;
    const auto now = GetTickCount64();
    const auto run = m_pFilter->m_featureOsdRun.load(std::memory_order_relaxed);
    Result result;
    {
        std::lock_guard guard(m_featureStatusMutex);
        const auto perceptual = m_perceptual.draws;
        if (m_featureStatusOsdRun != run) {
            m_featureStatusOsdRun = run;
            m_featureNotice.Reset();
            m_featureStatusLastPerceptual = perceptual;
        }
        const bool newPerceptual = perceptual > m_featureStatusLastPerceptual;
        m_featureStatusLastPerceptual = perceptual;
        const bool srSelected = m_iVPSuperRes == SUPERRES_DLSS_SR;
        const bool projected = CanProjectJvr();
        const bool sdr = !SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_pPSHalfOUtoInterlace &&
            m_currentSwapChainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        Request request{};
        request.srMode = m_iVPSuperRes;
        request.srLimit = srSelected ? m_dlssSRSourceLimit : 0;
        request.mfg = m_mfgMultiplier.load(std::memory_order_relaxed);
        if (m_pFilter->m_videoEnhancement.load()) request.flags |= Color;
        if (m_pFilter->m_adaptiveTone.load()) request.flags |= Tone;
        if (m_pFilter->m_memoryColor.load()) request.flags |= Memory;
        if (m_pFilter->m_skinTone.load()) request.flags |= Skin;
        if (m_jvrFlatView) request.flags |= Flat;
        Result observed{};
        // Only actual AI output dimensions; ordinary window scaling is never a result.
        // The VSR record is a successful VP enlargement, not a requested destination.
        const Size source{int(m_srcRect.Width()), int(m_srcRect.Height())};
        if (srSelected && !m_dlssSRInputStatus && !projected) {
            const auto sr = m_dlssSR.GetStats();
            if (Size{int(sr.inputWidth),int(sr.inputHeight)} == source) {
                const Size out{int(sr.outputWidth),int(sr.outputHeight)};
                if (Upscaled(source,out)) { observed.input=source; observed.output=out; }
            }
        } else if (!srSelected && request.srMode!=SUPERRES_Disable && m_VendorId==PCIV_NVIDIA && m_bVPUseSuperRes && !projected && m_editionVsr.tick &&
                   now>=m_editionVsr.tick && now-m_editionVsr.tick<=1000 &&
                   Size{int(m_editionVsr.input.cx),int(m_editionVsr.input.cy)}==source) {
            const Size out{int(m_editionVsr.output.cx),int(m_editionVsr.output.cy)};
            if (Upscaled(source,out)) { observed.input=source; observed.output=out; }
        }
        // The corner notice describes the configured cadence goal, not a
        // measured rate. Use nominal media timing, the refresh target and the
        // existing capability/budget policy, never per-pair slots or Present counts.
        Result cadence{};
        if (request.mfg>=2 && m_rtAvgTimePerFrame>0 && m_dRefreshPeriodMs>0.0 && !m_pPSHalfOUtoInterlace) {
            const double sourceFps=double(UNITS)/m_rtAvgTimePerFrame;
            const double refresh=1000.0/m_dRefreshPeriodMs;
            const unsigned cap=m_mfg.MaxAvailableMultiplier();
            const unsigned multiplier=request.mfg==mfg::AutoMultiplier ? cap :
                (std::min)(cap,mfg::DisplayBudgetMultiplier(request.mfg,sourceFps,refresh));
            SetCadence(cadence,sourceFps,refresh,multiplier);
        }
        // A target change (e.g. display refresh) may rearm the notice. A transient
        // bypass/failure does not alter the configured target or replay it.
        request.targetFromFps=cadence.fromFps;
        request.targetToFps=cadence.toFps;
        if (cadence.toFps && !m_mfgPresentFailed.load(std::memory_order_relaxed) && !m_mfg.Failed() && !MfgSourceMeetsDisplayRate()) {
            observed.fromFps=cadence.fromFps;
            observed.toFps=cadence.toFps;
        }
        if (sdr) {
            if ((request.flags & Color) && !m_svdLutRawTest.failed &&
                m_svdLutRawTest.activationPts != INVALID_TIME) observed.flags |= Color;
            if ((request.flags & Tone) && !m_adaptiveTone.shaderFailed && m_adaptiveTone.applied) observed.flags |= Tone;
            if (newPerceptual) observed.flags |= request.flags & (Memory | Skin);
        }
        if (projected && m_featureProjectionReady && m_jvrShader && m_jvrFrame.pTexture) observed.flags |= Flat;
        m_featureNotice.Observe(request,observed,now);
        // Statistics take precedence. Acknowledged notices are not replayed
        // when Statistics are switched off.
        if (m_bShowStats) { m_featureNotice.Suppress(); return 0; }
        result=m_featureNotice.Visible(now);
    }
    if (result.Empty()) return 0;
    const auto main = MainText(result);
    const auto indicators = IndicatorText(result.flags);
    if (main.empty() && indicators.empty()) return 0;
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    const CSize size(static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height));
    if (size.cx < 128 || size.cy < 64) return 0;
    // Scale against the actual presentation surface, independent of source and
    // Statistics font. Round to even pixels to limit atlas churn on resize.
    const LONG nominal = (std::min)(size.cy/40, size.cx/48);
    const LONG fontHeight = (std::clamp)((nominal+1)&~1L,18L,72L);
    const LONG margin = (std::max)(8L,fontHeight/3);
    const LONG padX = (std::max)(6L,fontHeight/3);
    const LONG padY = (std::max)(4L,fontHeight/5);
    const LONG gap = (std::max)(2L,fontHeight/8);
    if (m_featureOsdResourceStatus == S_FALSE) {
        HRESULT hr = m_featureOsdFont.InitDeviceObjects(m_pDevice,m_pDeviceContext);
        if (SUCCEEDED(hr)) hr = m_featureOsdBackground.InitDeviceObjects(m_pDevice,m_pDeviceContext);
        m_featureOsdResourceStatus = SUCCEEDED(hr) ? S_OK : hr;
        if (FAILED(hr)) {
            m_featureOsdFont.InvalidateDeviceObjects();
            m_featureOsdBackground.InvalidateDeviceObjects();
        }
    }
    if (FAILED(m_featureOsdResourceStatus)) return 0;
    if (FAILED(m_featureOsdFont.CreateFontBitmap(L"Consolas",fontHeight,0))) return 0;
    SIZE mainSize{}, indicatorSize{};
    if (!main.empty() && FAILED(m_featureOsdFont.GetTextExtent(main.c_str(), &mainSize))) return 0;
    if (!indicators.empty() && FAILED(m_featureOsdFont.GetTextExtent(indicators.c_str(), &indicatorSize))) return 0;
    const LONG spacing = !main.empty() && !indicators.empty() ? gap : 0;
    const LONG width=(std::max)(mainSize.cx,indicatorSize.cx)+2*padX;
    const LONG height=mainSize.cy+indicatorSize.cy+spacing+2*padY;
    if (width+2*margin>size.cx || height+2*margin>size.cy) return 0;
    CComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(m_pDevice->CreateRenderTargetView(target,nullptr,&rtv))) return 0;
    const RECT panel{margin,margin,margin+width,margin+height};
    HRESULT drawn=m_featureOsdBackground.Set(panel,size,D3DCOLOR_ARGB(204,0,0,0));
    if (SUCCEEDED(drawn)) drawn=m_featureOsdBackground.Draw(rtv,size);
    if (SUCCEEDED(drawn) && !main.empty()) drawn=m_featureOsdFont.Draw2DText(rtv,size,margin+padX,margin+padY,
        D3DCOLOR_XRGB(255,255,255),main.c_str());
    if (SUCCEEDED(drawn) && !indicators.empty()) drawn=m_featureOsdFont.Draw2DText(rtv,size,margin+padX,
        margin+padY+mainSize.cy+spacing,D3DCOLOR_XRGB(117,214,164),indicators.c_str());
    if (SUCCEEDED(drawn)) {
        std::lock_guard guard(m_featureStatusMutex);
        m_featureNotice.Drawn(now);
    }
    return SUCCEEDED(drawn) ? height+margin : 0;
}
