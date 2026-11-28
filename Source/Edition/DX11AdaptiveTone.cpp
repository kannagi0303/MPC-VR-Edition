// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
// [AT-03-FIX01 | 2026-11-25 11:44 +08:00] Native renderer header prerequisites.
#include "Helper.h"
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"
#include "Shaders.h"
#include "Edition/AdaptiveToneShader.h"
#include <thread>
#include <d3dcompiler.h>
#include <cstring>

// [AT-03 | 2026-11-25 11:25 +08:00] Optional analysis and composition.
// All GPU context calls stay on the existing serialized renderer path.
namespace {
using namespace edition::adaptive_tone;
static_assert(SampleWidth==256 && SampleHeight==144);
constexpr auto Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr int64_t Interval=800'000;
bool Supported(DXGI_FORMAT f) {return f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_B8G8R8X8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R10G10B10A2_UNORM;}
bool PinAdaptiveToneModule() {
    static const bool pinned=[] { HMODULE module=nullptr; return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&PinAdaptiveToneModule),&module)!=FALSE; }();
    return pinned;
}
}
bool CDX11VideoProcessor::AdaptiveToneSupported() const {
    return m_pFilter->m_adaptiveTone.load() && !SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_pPSHalfOUtoInterlace && m_currentSwapChainColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}
bool CDX11VideoProcessor::OptionalColorSupported() const {return AdaptiveToneSupported();}
void CDX11VideoProcessor::ResetAdaptiveTone(bool release) {
    auto& a=m_adaptiveTone;
    a.shot.Reset(); a.requestPts=a.uploadedPts=INVALID_TIME; a.allowDraw=a.applied=a.sourceComposition=false;
    // An old asynchronous copy may still own this staging allocation. On seek
    // leave it pending, but discard its result; reuse only after DO_NOT_WAIT succeeds.
    a.capturePts=INVALID_TIME; a.capturePrimed=false; a.primeTick=0;
    a.captureEarly=a.captureSpeculative=false; a.captureEpoch=0;
    a.previousRequestPts=INVALID_TIME;
    a.suspended=a.captureAfterSuspend=false;
    if(release) {
        ClearEarlyOriginal(true);
        a.sourceOutput.Release();
        a.sourceRaw.Release();
        a.sourceVsrOutput.Release();
        a.sourceCscVP.ReleaseVideoDevice();
        a.sourceVsrVP.ReleaseVideoDevice();
        a.sourceVsrKey = {};
        a.sourceVsrReady = false;
        a.srSourceToneRoute.reset();
        a.sourceToneBeforeUpscale=false;
        a.pending=false; a.thumbnail.Release(); a.staging.Release();
        a.curve.Release(); a.constants.Release(); a.sampleConstants.Release(); a.shader.Release(); a.sampleShader.Release(); a.job.reset(); a.shaderFailed=false;
    }
}
void CDX11VideoProcessor::StartAdaptiveToneShader() {
    auto& a=m_adaptiveTone;
    if(a.job || a.shader || a.shaderFailed) return;
    if(!PinAdaptiveToneModule()){a.shaderFailed=true;return;}
    // [FA-02 | 2026-11-25 14:36 +08:00] CPU bytecode survives device and video changes.
    try {
        static const auto cached=[] {
            auto job=std::make_shared<AdaptiveToneShaderJob>();
            // CPU-only compile. No renderer, device or Immediate Context captured;
            // shutdown/seek never joins or waits for this work.
            std::thread([job]{
                SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
                // Analysis is upstream of the output shader. Publish its
                // bytecode first; both consumers still create device-local objects.
                try { CompileShader(AdaptiveToneThumbnail,nullptr,"ps_5_0",&job->sampleCode); } catch(...) {}
                job->sampleReady.store(true,std::memory_order_release);
                try { CompileShader(std::string(AdaptiveToneShader)+AdaptiveToneStandalone,nullptr,"ps_5_0",&job->code); } catch(...) {}
                job->ready.store(true,std::memory_order_release);
            }).detach();
            return job;
        }();
        a.job=cached;
    } catch(...) {a.shaderFailed=true;}
}
bool CDX11VideoProcessor::AdaptiveToneCaptureAllowed(REFERENCE_TIME pts,bool buffered) {
    const auto& a=m_adaptiveTone;
    return OptionalColorSupported() && pts>=0 && m_pFilter->m_filterState==State_Running &&
        !m_pFilter->m_bFlushing && !m_fastShutdown && !m_completedPresentFairness.Active() &&
        !a.pending && (a.requestPts==INVALID_TIME || pts-a.requestPts>=Interval) &&
        VideoEnhancementMayStart(buffered);
}
void CDX11VideoProcessor::PollAdaptiveTone(REFERENCE_TIME pts,REFERENCE_TIME primeLimit) {
    auto& a=m_adaptiveTone;
    if(!OptionalColorSupported() || m_pFilter->m_bFlushing || m_fastShutdown ||
        m_pFilter->m_filterState!=State_Running) return;
    if(m_completedPresentFairness.Active() || CompletedGeometryChanging()) {
        a.suspended=true; // even if no outstanding readback
        return;
    }
    const bool priming=primeLimit!=INVALID_TIME;
    if(a.captureEarly && (a.captureEpoch!=m_pFilter->m_completedEpoch ||
        m_pFilter->m_playbackCancel.Capture()->Cancelled())) {
        // A segment can change before the old early slot is visited again.
        // Retire its copy without trusting either its timestamp or cadence.
        a.capturePts=a.requestPts=INVALID_TIME;
        a.captureEarly=a.captureSpeculative=false; a.captureEpoch=0;
        a.previousRequestPts=INVALID_TIME;
        a.suspended=a.suspended || a.captureAfterSuspend;
        a.capturePrimed=a.captureAfterSuspend=false;
    }
    if(!a.pending || a.captureSpeculative)return;
    // Only the initial, not-yet-locked anchor may consume future observations.
    // Ordinary presentation never consumes an observation beyond its PTS.
    const bool prime=priming && a.capturePrimed;
    if(a.capturePts!=INVALID_TIME && a.capturePts>(prime?primeLimit:pts))return;
    D3D11_MAPPED_SUBRESOURCE map{};
    const auto hr=m_pDeviceContext->Map(a.staging.pTexture,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
    if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return;
    a.pending=false;
    if(SUCCEEDED(hr)) {
        if(a.capturePts!=INVALID_TIME && map.RowPitch>=SampleWidth*4*sizeof(float)) {
            const auto sample=ReadObservation(static_cast<const float*>(map.pData),map.RowPitch/sizeof(float));
            if(AdaptiveToneSupported()) {
                a.shot.Observe(sample,a.capturePts,a.captureAfterSuspend,a.capturePrimed?pts:INVALID_TIME);
            }
        }
        m_pDeviceContext->Unmap(a.staging.pTexture,0);
        
    }
    if(FAILED(hr) && a.captureAfterSuspend) a.suspended=true;
    a.capturePts=INVALID_TIME; a.capturePrimed=a.captureAfterSuspend=false;
    a.captureEarly=a.captureSpeculative=false; a.captureEpoch=0;
    a.previousRequestPts=INVALID_TIME;
}
void CDX11VideoProcessor::ObserveAdaptiveTone(const Tex2D_t& source,const CRect& sample,REFERENCE_TIME pts,bool buffered,bool primed) {
    if(!AdaptiveToneCaptureAllowed(pts,buffered) || !source.pShaderResource || !Supported(source.desc.Format) ||
        sample.left<0 || sample.top<0 || sample.right>LONG(source.desc.Width) || sample.bottom>LONG(source.desc.Height) || sample.Width()<2 || sample.Height()<2) return;
    auto& a=m_adaptiveTone;
    StartAdaptiveToneShader();
    if(!a.sampleShader) {
        if(a.shaderFailed || !a.job || !a.job->sampleReady.load(std::memory_order_acquire)) return;
        if(!a.job->sampleCode || FAILED(m_pDevice->CreatePixelShader(a.job->sampleCode->GetBufferPointer(),a.job->sampleCode->GetBufferSize(),nullptr,&a.sampleShader))) {a.shaderFailed=true;return;}
    }
    a.requestPts=pts; // bounded attempts, including allocation failures
    if(!a.sampleConstants) {D3D11_BUFFER_DESC cb{};cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cb.ByteWidth=16;if(FAILED(m_pDevice->CreateBuffer(&cb,nullptr,&a.sampleConstants))) return;}
    const float rect[4]={float(sample.left),float(sample.top),float(sample.Width()),float(sample.Height())};
    m_pDeviceContext->UpdateSubresource(a.sampleConstants,0,nullptr,rect,0,0);
    if(FAILED(a.thumbnail.CheckCreate(m_pDevice,Format,SampleWidth,SampleHeight,Tex2D_DefaultShaderRTarget)) ||
        FAILED(a.staging.CheckCreate(m_pDevice,Format,SampleWidth,SampleHeight,Tex2D_StagingRead))) return;
    m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
    const auto hr=TextureCopyRect(source,a.thumbnail.pTexture,sample,CRect(0,0,SampleWidth,SampleHeight),a.sampleShader,a.sampleConstants,0,false);
    m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
    if(FAILED(hr)) return;
    m_pDeviceContext->CopyResource(a.staging.pTexture,a.thumbnail.pTexture);
    a.pending=true; a.capturePts=pts; a.capturePrimed=primed;
    a.captureAfterSuspend=a.suspended; a.suspended=false;
}
// FIX19: move an existing observation into the already admitted early RGB
// preparation. Never allocate new GPU resources or replace a pending readback.
// A future original may not take the sampling opportunity of its predecessor.
void CDX11VideoProcessor::PrepareEarlyAdaptiveTone(const Tex2D_t& source, const CRect& picture,
    REFERENCE_TIME pts, uint64_t epoch, REFERENCE_TIME precedingPts)
{
    auto& a=m_adaptiveTone;
    if (!a.sampleShader || !a.sampleConstants || !a.thumbnail.pTexture || !a.staging.pTexture ||
        a.pending || a.captureSpeculative || epoch != m_pFilter->m_completedEpoch ||
        m_pFilter->m_playbackCancel.Capture()->Cancelled()) return;
    if (precedingPts>=0 && (a.requestPts==INVALID_TIME || precedingPts-a.requestPts>=Interval)) return;
    const auto previous=a.requestPts;
    ObserveAdaptiveTone(source,picture,pts,true);
    if (a.pending && a.capturePts==pts) {
        a.captureEarly=a.captureSpeculative=true;
        a.captureEpoch=epoch;
        a.previousRequestPts=previous;
    } else {
        // A failed speculative attempt must not suppress normal sampling.
        a.requestPts=previous;
    }
}

void CDX11VideoProcessor::ConfirmEarlyAdaptiveTone(const Tex2D_t& source, const CRect& picture, REFERENCE_TIME pts)
{
    auto& a=m_adaptiveTone;
    if (!a.captureSpeculative || a.capturePts!=pts) return;
    const auto& e=m_earlyOriginal;
    if (e.ready && e.approved && e.consumed && e.pts==pts && e.epoch==a.captureEpoch &&
        e.epoch==m_pFilter->m_completedEpoch && e.rgb.pTexture.p==source.pTexture.p &&
        picture==CRect(0,0,source.desc.Width,source.desc.Height) &&
        !m_pFilter->m_playbackCancel.Capture()->Cancelled()) {
        a.captureSpeculative=false;
        a.previousRequestPts=INVALID_TIME;
    } else DiscardEarlyAdaptiveTone(pts,a.captureEpoch);
}

void CDX11VideoProcessor::DiscardEarlyAdaptiveTone(REFERENCE_TIME pts, uint64_t epoch)
{
    auto& a=m_adaptiveTone;
    if (!a.captureSpeculative || a.capturePts!=pts || a.captureEpoch!=epoch) return;
    if (a.requestPts==pts) a.requestPts=a.previousRequestPts;
    // Keep pending true until the original DO_NOT_WAIT Map retires the GPU
    // copy. Never reuse staging memory while an abandoned copy owns it.
    a.suspended=a.suspended || a.captureAfterSuspend;
    a.capturePts=INVALID_TIME;
    a.capturePrimed=a.captureAfterSuspend=false;
    a.captureEarly=a.captureSpeculative=false;
    a.captureEpoch=0;
    a.previousRequestPts=INVALID_TIME;
}

// [AT-06 | 2026-11-25 12:18 +08:00] Borrow only READY originals in the
// existing presenter slack. No extra timer, wait, queue ownership or producer
// admission change. Never advance an already established shot into the future.
void CDX11VideoProcessor::PrimeAdaptiveToneFrames() {
    auto& a=m_adaptiveTone;
    const auto needPrime=[&]{return AdaptiveToneSupported() && a.shot.locked<0;};
    if(!needPrime() || m_completedFrames.empty() ||
        !m_completedFence || !m_pDeviceContext || m_pFilter->m_filterState!=State_Running ||
        m_pFilter->m_bFlushing || m_fastShutdown || m_completedPresentFairness.Active() ||
        CompletedGeometryChanging() || m_extendedStartupPending ||
        m_pFilter->m_completedSourceWaiting.load(std::memory_order_acquire) ||
        !m_completedBatches.empty()) return;
    CRefTime stream;
    if(FAILED(m_pFilter->StreamTime(stream)) ||
        m_completedFrames.front().due-REFERENCE_TIME(stream)<=50'000 ||
        GetCompletedBufferTime(stream).ready<250*10'000 || !VideoEnhancementMayStart(true)) return;
    const auto tick=GetTickCount64();
    if(a.primeTick && tick-a.primeTick<33) return;
    const auto finished=m_completedFence->GetCompletedValue();
    if(finished==UINT64_MAX) return;
    const auto& head=m_completedFrames.front();
    if(head.pts<0) return;
    constexpr REFERENCE_TIME lookahead=10'000'000; // at most 1 s source time
    a.primeTick=tick;
    PollAdaptiveTone(head.pts,head.pts+lookahead);
    if(!needPrime() || a.pending) return;
    // Chronological unique original samples, same picture domain. Capture at
    // most one per turn; the existing 80 ms media cadence still applies.
    for(const auto& frame:m_completedFrames) {
        // Do not jump over an unready frame or a change of picture domain.
        if(frame.sourceColorFinal || frame.pts<head.pts || frame.pts-head.pts>lookahead || frame.fence>finished ||
            frame.picture!=head.picture || frame.projected!=head.projected || frame.videoOnly!=head.videoOnly ||
            frame.image.desc.Width!=head.image.desc.Width || frame.image.desc.Height!=head.image.desc.Height ||
            frame.image.desc.Format!=head.image.desc.Format) break;
        if(frame.phase!=mfg::MaxGenerated || !frame.image.pShaderResource ||
            !AdaptiveToneCaptureAllowed(frame.pts,true)) continue;
        CRect sample;
        if(sample.IntersectRect(frame.picture,CRect(0,0,frame.image.desc.Width,frame.image.desc.Height))) {
            ObserveAdaptiveTone(frame.image,sample,frame.pts,true,true);
            break;
        }
    }
}
// [EXP-FADE-0] Source and legacy routes now share the same immediate
// activation policy. Analysis, anchor/PTS eligibility and dither stay unchanged.
float CDX11VideoProcessor::AdaptiveToneWeight(REFERENCE_TIME pts) const {
    return m_adaptiveTone.shot.Fade(pts) * (m_pFilter->m_adaptiveTone.load()/100.f);
}

// VE-SOURCE: only original-frame callers may enter the producer stage.
// Completed SDR originals are finalized even when controls are zero; a later
// settings change must not repaint previously queued originals or descendants.
bool CDX11VideoProcessor::SourceToneEligible() const {
    return ((m_completingSample && m_completedTarget) || PerceptualEnabled() || OptionalColorSupported()) &&
        !SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_pPSHalfOUtoInterlace &&
        m_currentSwapChainColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 &&
        !m_fastShutdown && !m_pFilter->m_bFlushing &&
        !m_pFilter->m_playbackCancel.Capture()->Cancelled();
}

ID3D11Texture2D* CDX11VideoProcessor::PrepareSourceTone(const Tex2D_t& source, const CRect& picture, REFERENCE_TIME pts, ID3D11Texture2D* directTarget) {
    auto& a=m_adaptiveTone;
    // An approved early thumbnail must describe exactly the raw RGB consumed
    // by the grade. Any alternate composition route invalidates it first.
    if (m_earlyOriginal.ready && m_earlyOriginal.approved &&
        (pts != m_earlyOriginal.pts || source.pTexture.p != m_earlyOriginal.rgb.pTexture.p))
        FinishEarlyOriginal();
    // A successor's speculative LUT is allowed to contribute only when the
    // current picture actually used its matching early RGB route. In a
    // fallback (different SR/VP route, projection, changed crop/ProcAmp, etc.) remove that
    // successor and dependent cached transforms BEFORE selecting a LUT.
    if (m_futureOriginal.ready &&
        (!m_earlyOriginal.consumed || !m_earlyOriginal.approved ||
         pts != m_earlyOriginal.pts || source.pTexture.p != m_earlyOriginal.rgb.pTexture.p ||
         picture != CRect(0,0,source.desc.Width,source.desc.Height) ||
         m_futureOriginal.srSource != m_earlyOriginal.srSource ||
         !EarlyOriginalDomainMatches(m_earlyOriginal) || !EarlyOriginalDomainMatches(m_futureOriginal) ||
         !EarlyOriginalRoute(m_srcRect, ProcessingRect()))) {
        DiscardEarlyOriginal(m_futureOriginal);
        m_futureOriginal = {};
    }
    auto* raw=source.pTexture.p;
    const bool perceptual=PerceptualEnabled();
    const bool adaptive=OptionalColorSupported();
    if(!raw || !source.pShaderResource || (!perceptual && !adaptive) || pts<0 ||
        !Supported(source.desc.Format) || !SourceToneEligible()) return raw;
    const CRect area(0,0,source.desc.Width,source.desc.Height);
    CRect visible;
    if(!visible.IntersectRect(picture,area) || visible.Width()<2 || visible.Height()<2) return raw;
    m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
    ID3D11ShaderResourceView* unbindSource=nullptr;
    m_pDeviceContext->PSSetShaderResources(0,1,&unbindSource);
    // Capture ungraded originals at the native geometry. The existing depth
    // producer deduplicates by PTS; SR/MFG may also request that same original.
    const auto modelSize=ProcessingSourceRect(CanProjectJvr()).Size();
    const bool fullOriginal=visible==area && visible==picture && area.Size()==modelSize &&
        MfgDepthSourceEligible() && !m_bInterlaced;
    if(fullOriginal && (UsesDepthSr() || UsesDepthMfg()))
        m_depth.Update(m_pDevice,m_pDeviceContext,source.pShaderResource,visible,modelSize,pts);
    if(UsesDepthSr() || UsesDepthMfg()) m_depth.PollCompleted();
    a.allowDraw=adaptive; a.applied=false; a.sourceComposition=true;
    struct Restore { bool& draw; bool& source; ~Restore(){draw=false;source=false;} } restore{a.allowDraw,a.sourceComposition};
    if(adaptive) {
        ConfirmEarlyAdaptiveTone(source,visible,pts);
        PollAdaptiveTone(pts);
    }
    // Observation queues a readback; it does not change the current shot/curve.
    // Preserve it even on a neutral frame or output allocation failure.
    const auto observeTone=[&] {
        if(adaptive) ObserveAdaptiveTone(source,visible,pts,false);
    };
    const bool tone=adaptive && AdaptiveToneWeight(pts)>0;
    if(!perceptual && !tone) { observeTone(); return raw; }
    if(!directTarget) {
        if(FAILED(a.sourceOutput.CheckCreate(m_pDevice,source.desc.Format,source.desc.Width,source.desc.Height,Tex2D_DefaultShaderRTarget))) {
            observeTone();
            return raw;
        }
        directTarget=a.sourceOutput.pTexture.p;
    }
    if(!directTarget || directTarget==raw) { observeTone(); return raw; }
    // B1/B2.1/B2.2 remain fused with Adaptive Tone and perceptual colour.
    // The current original's asynchronous thumbnail may not be ready; never
    // stall the decoder, and never grade the MFG descendants to compensate.
    auto& b=m_svdLutRawTest;
    b.active=false;
    HRESULT hr=S_FALSE;
    const bool allowDepth=fullOriginal || (visible==area && visible==picture &&
        MfgDepthSourceEligible() && !CanProjectJvr() && m_renderRect==m_videoRect &&
        m_depth.MatchesColorSource(modelSize.cx,modelSize.cy));
    bool observedWithLut=false;
    if(m_pFilter->m_videoEnhancement.load() && !b.failed &&
        (b.activationPts!=INVALID_TIME || VideoEnhancementMayStart(false))) {
        if(!b.attempted) StartSvdLutRawTest();
        if(!b.failed && b.model.GetState()==svdlut::ModelWorker::State::Ready) {
            if(FAILED(PrepareSvdLutRawResources())) b.failed=true;
            else {
                // The actual ungraded RGB is the analysis input. Capture
                // and grade are separate entry points, preserving FIX10's
                // order: poll/capture -> AT observation -> poll/select/draw.
                observedWithLut=true;
                const bool pausedReplay=BeginSvdLutAnalysis(source,visible,pts,0,true);
                if(a.sourceComposition && a.allowDraw) ObserveAdaptiveTone(source,visible,pts,false);
                hr=ApplySvdLutRawFrame(source,directTarget,area,visible,CPoint(0,0),pts,0,true,allowDepth,pausedReplay);
            }
        }
    }
    if(!observedWithLut) observeTone();
    if(hr!=S_OK && perceptual)
        hr=ApplyPerceptual(source,directTarget,area,visible,CPoint(0,0),pts,
            0,nullptr,{1.0f,1.0f,1.0f},false,1.0f,allowDepth);
    if(hr!=S_OK && tone)
        hr=DrawAdaptiveTone(source,directTarget,area,area,pts,&visible);
    m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
    ID3D11ShaderResourceView* empty=nullptr;
    m_pDeviceContext->PSSetShaderResources(0,1,&empty);
    return hr==S_OK ? directTarget : raw;
}

float CDX11VideoProcessor::BindAdaptiveTone(REFERENCE_TIME pts) {
    auto& a=m_adaptiveTone;
    if(!a.allowDraw || !AdaptiveToneSupported()) return 0.f;
    const float weight=AdaptiveToneWeight(pts);
    if(weight<=0) return 0.f;
    if(!a.curve) {
        D3D11_BUFFER_DESC cb{}; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cb.ByteWidth=65*4*sizeof(float);
        if(FAILED(m_pDevice->CreateBuffer(&cb,nullptr,&a.curve))) return 0.f;
    }
    if(a.uploadedPts!=a.shot.locked) {
        std::array<float,65*4> values{}; std::copy(a.shot.curve.begin(),a.shot.curve.end(),values.begin());
        m_pDeviceContext->UpdateSubresource(a.curve,0,nullptr,values.data(),0,0); a.uploadedPts=a.shot.locked;
    }
    m_pDeviceContext->PSSetConstantBuffers(1,1,&a.curve.p); return weight;
}
void CDX11VideoProcessor::UnbindAdaptiveTone(){ID3D11Buffer* empty=nullptr;m_pDeviceContext->PSSetConstantBuffers(1,1,&empty);}
HRESULT CDX11VideoProcessor::DrawAdaptiveTone(const Tex2D_t& source,ID3D11Texture2D* target,const CRect& src,const CRect& dst,REFERENCE_TIME pts,const CRect* mask,bool scaled) {
    auto& a=m_adaptiveTone;
    if(!a.allowDraw || !OptionalColorSupported() || !target || !source.pShaderResource || (!scaled && src.Size()!=dst.Size()) || AdaptiveToneWeight(pts)<=0) return S_FALSE;
    if(scaled) {
        D3D11_TEXTURE2D_DESC desc{}; target->GetDesc(&desc);
        if(!Supported(desc.Format) || !Supported(source.desc.Format) || src.IsRectEmpty() || dst.IsRectEmpty()) return S_FALSE;
    }
    if(!a.shader) {
        if(a.shaderFailed || !a.job || !a.job->ready.load(std::memory_order_acquire)) return S_FALSE;
        if(!a.job->code || FAILED(m_pDevice->CreatePixelShader(a.job->code->GetBufferPointer(),a.job->code->GetBufferSize(),nullptr,&a.shader))) a.shaderFailed=true;
        a.job.reset(); if(!a.shader) return S_FALSE;
    }
    if(!a.constants) {D3D11_BUFFER_DESC cb{};cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cb.ByteWidth=48;if(FAILED(m_pDevice->CreateBuffer(&cb,nullptr,&a.constants))) return S_FALSE;}
    const float weight=BindAdaptiveTone(pts); if(weight<=0) return S_FALSE;
    const CRect& clip=mask?*mask:dst;
    const float cb[12]={float(src.left-dst.left),float(src.top-dst.top),weight,BindSvdLutOutputDither(target,!a.sourceComposition),float(clip.left),float(clip.top),float(clip.right),float(clip.bottom),scaled?1.f:0.f,0,0,0};
    m_pDeviceContext->UpdateSubresource(a.constants,0,nullptr,cb,0,0);
    const auto hr=TextureCopyRect(source,target,src,dst,a.shader,a.constants,0,false,scaled);
    UnbindAdaptiveTone(); ID3D11ShaderResourceView* empty=nullptr; m_pDeviceContext->PSSetShaderResources(4,1,&empty);
    a.applied=hr==S_OK; return hr;
}
bool CDX11VideoProcessor::CopyAdaptiveTonePicture(ID3D11Texture2D* target,const CRect& picture,CRect& visible,CRect& local) {
    if(!target) return false;
    D3D11_TEXTURE2D_DESC d{};target->GetDesc(&d);
    if(!Supported(d.Format) || !visible.IntersectRect(picture,CRect(0,0,d.Width,d.Height)) || visible.Width()<2 || visible.Height()<2 ||
        FAILED(EnsureSvdLutPicture(d.Format,visible.Width(),visible.Height()))) return false;
    local.SetRect(0,0,visible.Width(),visible.Height());
    const D3D11_BOX box{UINT(visible.left),UINT(visible.top),0,UINT(visible.right),UINT(visible.bottom),1};
    m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
    m_pDeviceContext->CopySubresourceRegion(m_svdLut.picture.pTexture,0,0,0,0,target,0,&box);
    return true;
}
void CDX11VideoProcessor::DrawAdaptiveToneTarget(ID3D11Texture2D* target,const CRect& picture,REFERENCE_TIME pts) {
    const auto& a=m_adaptiveTone;
    if(!a.allowDraw || a.applied || AdaptiveToneWeight(pts)<=0 || a.shaderFailed || m_completedPresentFairness.Active() || CompletedGeometryChanging() ||
        (!a.shader && (!a.job || !a.job->ready.load(std::memory_order_acquire)))) return;
    CRect visible,local;
    if(CopyAdaptiveTonePicture(target,picture,visible,local)) DrawAdaptiveTone(m_svdLut.picture,target,local,visible,pts);
}
void CDX11VideoProcessor::BeginAdaptiveToneLegacy(ID3D11Texture2D* target,const CRect& picture,REFERENCE_TIME pts,bool allowCapture) {
    auto& a=m_adaptiveTone; a.allowDraw=!a.sourceToneBeforeUpscale && OptionalColorSupported();a.applied=false;
    if(!a.allowDraw) return;
    PollAdaptiveTone(pts);
    if(!allowCapture || !AdaptiveToneCaptureAllowed(pts,false)) return;
    D3D11_TEXTURE2D_DESC desc{};
    if(target) {
        target->GetDesc(&desc);
        if((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) && Supported(desc.Format)) {
            Tex2D_t source; source.pTexture=target; source.desc=desc;
            m_pDeviceContext->OMSetRenderTargets(0,nullptr,nullptr);
            if(SUCCEEDED(m_pDevice->CreateShaderResourceView(target,nullptr,&source.pShaderResource))) {
                CRect sample;
                if(sample.IntersectRect(picture,CRect(0,0,desc.Width,desc.Height))) ObserveAdaptiveTone(source,sample,pts,false);
                return;
            }
        }
    }
    // Legacy target may not expose an SRV. Use the existing reusable colour
    // surface, only at capture cadence, not a synchronous GPU readback.
    CRect visible,local;
    if(CopyAdaptiveTonePicture(target,picture,visible,local)) ObserveAdaptiveTone(m_svdLut.picture,local,pts,false);
}
