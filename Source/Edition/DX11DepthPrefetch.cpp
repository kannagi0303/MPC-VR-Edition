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
#include <new>
#include <ID3DVideoMemoryConfiguration.h>

namespace {
class LookaheadSample final : public CMediaSample, public IMediaSampleD3D11 {
    CComPtr<CMemAllocator> m_allocator;
    CComPtr<ID3D11Texture2D> m_texture;
public:
    LookaheadSample(CMemAllocator* allocator, ID3D11Texture2D* texture)
        : CMediaSample(L"depth lookahead sample", allocator, nullptr), m_allocator(allocator), m_texture(texture) {}

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
    if (CanProjectJvr() || !m_pDeviceContext || !m_pDevice || (!UsesDepthMfg() && !UsesDepthSr()) ||
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
        auto* allocator = new(std::nothrow) CMemAllocator(L"depth lookahead", nullptr, &hr);
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

bool CDX11VideoProcessor::PrefetchDepth(IMediaSample* sample, int64_t pts)
{
    if (CanProjectJvr() || (!UsesDepthMfg() && !UsesDepthSr()) || !m_pDevice || !m_pDeviceContext || !MfgDepthSourceEligible())
        return true;
    CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &texture, &slice)) || !texture || slice) return true;
    CComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return true;
    return m_depth.PrefetchNv12(m_pDevice, m_pDeviceContext, texture, m_srcRect,
        m_srcExFmt.NominalRange == DXVA2_NominalRange_0_255, pts);
}


void CDX11VideoProcessor::AnalyzeEditionDecodedFrame(HRESULT hr, bool comparedCpuNv12)
{
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

}

bool CDX11VideoProcessor::AnalyzeEditionCpuNv12(const BYTE* data, HRESULT hr)
{
			if (SUCCEEDED(hr) && !CanProjectJvr() && MfgFrameAnalysisEligible() && m_srcDXGIFormat == DXGI_FORMAT_NV12 &&
			    m_srcPitch > 0 && m_srcLines >= m_srcHeight + m_srcHeight / 2) {
				m_mfgDecodedDifference.SubmitCpuNv12(data, m_srcPitch, m_srcHeight, m_srcRect, m_rtStart, m_mfgLowMotionEnabled);
				return true;
			}
	return false;
}


// FIX16: use a separate, progressive CSC VP so an arriving decoder sample
// cannot modify the ordinary VP's frame history, rectangles or input index.
// Native grade route only; leave active SR/VSR, JVR and hardware scaling on
// their established paths until their resource contracts are integrated.
bool CDX11VideoProcessor::EarlyOriginalRoute(const CRect& source, const CRect& destination)
{
    if (!m_pDevice || !m_pDeviceContext || !m_D3D11VP.IsReady() ||
        m_srcDXGIFormat != DXGI_FORMAT_NV12 || m_srcParams.CDepth != 8 ||
        m_D3D11OutputFmt != DXGI_FORMAT_B8G8R8A8_UNORM || m_bInterlaced || m_bDoubleFrames ||
        m_SampleFormat != D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE ||
        m_iRotation || m_bFlip || CanProjectJvr() || m_Dovi.bValid ||
        m_bVPUseSuperRes || m_bVPRTXVideoHDR || !SourceToneEligible() || DepthOverlayEnabled() ||
        !m_pFilter->m_videoEnhancement.load() || source != m_srcRect || source.IsRectEmpty() ||
        destination.IsRectEmpty() ||
        (m_iVPSuperRes != SUPERRES_Disable && m_iVPSuperRes != SUPERRES_DLSS_SR)) return false;
    const bool sr = CanDlssSRUpscale(source, destination, false);
    // Only original-first SR owns the same native RGB conversion as the
    // normal TryDlssSR path. Post-MFG SR/native scaling keep their own input.
    if (sr) {
        if (!UpscaleAt(UpscaleSlot::BeforeMfg)) return false;
    } else if ((m_bVPScaling && destination.Size() != source.Size()) ||
        (destination.Width() > source.Width() && destination.Height() > source.Height())) return false;
    const auto& p = m_DXVA2ProcAmpValues;
    return p.Brightness.ll == 0 && p.Hue.ll == 0 &&
        p.Contrast.ll == 65536 && p.Saturation.ll == 65536;
}

bool CDX11VideoProcessor::EarlyOriginalDomainMatches(const EarlyOriginalImage& image) const
{
    const std::array<UINT, 5> key{UINT(m_srcDXGIFormat), UINT(m_srcWidth), UINT(m_srcHeight),
        UINT(m_srcExFmt.value), UINT(m_D3D11OutputFmt)};
    return image.ready && image.epoch == m_pFilter->m_completedEpoch &&
        image.rect == m_srcRect && image.sourceKey == key;
}

bool CDX11VideoProcessor::CanStageOriginal(IMediaSample* sample)
{
    if (!sample || !m_pDevice) return false;
    // Same device and exact supported decoder domain as FIX16. Do not
    // acknowledge a sample asynchronously if it may require a new CSC route.
    if (CComQIPtr<IMediaSideData> side = sample) {
        const BYTE* data = nullptr;
        size_t size = 0;
        if (SUCCEEDED(side->GetSideData(IID_MediaSideDataDOVIMetadataV2, &data, &size)) && size) return false;
    }
    CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &texture, &slice)) || !texture) return false;
    CComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    return desc.Format == DXGI_FORMAT_NV12 && desc.MipLevels == 1 && slice < desc.ArraySize &&
        desc.Width >= m_srcWidth && desc.Height >= m_srcHeight && desc.SampleDesc.Count == 1;
}

void CDX11VideoProcessor::ClearEarlyOriginal(bool release)
{
    auto& e = m_earlyOriginal;
    DiscardEarlyAdaptiveTone(e.pts,e.epoch);
    if (release) DiscardEarlyAdaptiveTone(m_futureOriginal.pts,m_futureOriginal.epoch);
    e.sample = nullptr;
    e.decoderTexture = nullptr;
    e.decoderSlice = 0;
    e.sourceKey = {};
    e.pts = INVALID_TIME;
    e.epoch = 0;
    e.ready = e.approved = e.consumed = e.captured = e.fromFuture = false;
    // Return the allocation to the exact scratch owner that lent it, not
    // whichever route is configured now. D3D11 retains submitted references.
    if (!release && e.rgb.pTexture) {
        auto& normal = e.srSource ? m_dlssSRSource : m_adaptiveTone.sourceRaw;
        if (!normal.pTexture) std::swap(e.rgb, normal);
        else e.rgb.Release();
    }
    e.srSource = false;
    if (release) {
        e.completion.Release();
        e.queryIssued = false;
        e.rgb.Release();
        e.vp.ReleaseVideoDevice();
        e.key = {};
        e.broken = false;
        e.prepared = e.reused = e.abandoned = e.captures = 0;
        e.futurePrepared = e.futureReused = 0;
        m_futureOriginal = {};
    }
}

void CDX11VideoProcessor::DiscardEarlyOriginal(EarlyOriginalImage& e)
{
    DiscardEarlyAdaptiveTone(e.pts,e.epoch);
    if (!e.ready) return;
    // A speculative CSC which is never used must not become a temporal
    // reference. Remove just this source and any cached transform that
    // could include it. Preserve unrelated history and source identity.
    if (!e.consumed) {
        ++m_earlyOriginal.abandoned;
        if (e.captured) {
            auto& b = m_svdLutRawTest;
            const auto pts = e.pts;
            const auto includes = [pts](const auto& span) {
                return span[0] >= 0 && span[0] <= pts && pts <= span[1];
            };
            for (auto& c : b.captures) {
                if (c.pts != pts || c.fence != 0) continue;
                c.pending = false;
                c.input.reset();
                c.pts = INVALID_TIME;
            }
            if (b.inFlight && b.inFlightPts == pts && b.inFlightFence == 0) {
                b.model.Invalidate();
                b.inFlight = false;
                b.inFlightPts = INVALID_TIME;
                b.inFlightFence = b.inFlightStart = 0;
            }
            bool invalidatedB2 = b.b2CachedSourcePts == pts;
            for (auto& f : b.frames) {
                if (f.pts == pts && f.fence == 0) f = {};
                else if (includes(f.temporalInputPts)) {
                    if (f.temporalView && b.b2CachedView.Get() == f.temporalView.Get()) invalidatedB2 = true;
                    f.temporalView.Reset();
                    f.temporalParameters.reset();
                    f.temporalFinalized = false;
                    f.temporalInputPts = {INVALID_TIME, INVALID_TIME};
                    f.temporalContributions = {};
                }
            }
            if ((b.heldPts == pts && b.heldFence == 0) || includes(b.heldInputPts)) {
                if (b.heldView && b.b2CachedView.Get() == b.heldView.Get()) invalidatedB2 = true;
                b.heldView.Reset();
                b.heldParameters.reset();
                b.heldPts = INVALID_TIME;
                b.heldFence = 0;
                b.heldInputPts = {INVALID_TIME, INVALID_TIME};
                b.heldContributions = {};
                b.heldSignatureReady = false;
            }
            if (invalidatedB2) {
                b.b2CachedView.Reset();
                b.b2CachedSourcePts = INVALID_TIME;
                b.b2CachedRefinedReady = false;
            }
        }
    }
}

void CDX11VideoProcessor::FinishEarlyOriginal()
{
    if (!m_earlyOriginal.ready) return;
    // A fallback can leave the producer route before PrepareSourceTone runs.
    // Do not let the unconfirmed successor survive an abandoned current slot.
    if (!m_earlyOriginal.consumed && m_futureOriginal.ready) {
        DiscardEarlyOriginal(m_futureOriginal);
        m_futureOriginal = {};
    }
    DiscardEarlyOriginal(m_earlyOriginal);
    ClearEarlyOriginal();
}

bool CDX11VideoProcessor::PrepareEarlyOriginal(IMediaSample* sample, REFERENCE_TIME pts, uint64_t epoch, bool future)
{
    auto& e = m_earlyOriginal;
    auto& b = m_svdLutRawTest;
    const auto ticket = m_pFilter->m_playbackCancel.Capture();
    // Return true when this Receive's preparation decision is complete
    // (success or permanent skip). False means an actual resource/deadline
    // condition may change on the existing admission retry.
    if (ticket->Cancelled() || !sample || e.ready || pts < 0 || epoch != m_pFilter->m_completedEpoch ||
        b.sourceTimeline.current < 0 || pts <= b.sourceTimeline.current ||
        m_processingSize.cx <= 0 || !EarlyOriginalRoute(m_srcRect, ProcessingRect()) ||
        b.failed || b.activationPts == INVALID_TIME ||
        b.model.GetState() != svdlut::ModelWorker::State::Ready ||
        !b.thumbnail.pTexture || !b.constants || !b.sampler) return true;
    if (!m_completedLast || !m_completedLast->sourceColorFinal || m_completedFrames.empty() ||
        !m_completedBatches.empty() || m_extendedStartupPending || m_completedPresentFairness.Active() ||
        m_resizePending || CompletedGeometryChanging() || !m_completedFence) return false;
    // The caller is already in completed_wait_lookahead. Borrow only a
    // fully signaled ready queue and its existing presentation deadline.
    // No new time constants or reduction in the configured buffer lead.
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return false;
    const auto done = m_completedFence->GetCompletedValue();
    if (done == UINT64_MAX || done < m_completedFenceValue ||
        m_completedFrames.front().due - REFERENCE_TIME(stream) <=
            CompletedBackBufferLead(m_completedFrames.front(), CompletedSubmitLead()) ||
        !std::all_of(m_completedFrames.begin(), m_completedFrames.end(),
            [done](const auto& f) { return f.sourceColorFinal && f.fence <= done; })) return false;
    for (const auto& frame : b.frames)
        if (frame.pts == pts && frame.fence == 0) return true;
    if (std::none_of(b.captures.begin(), b.captures.end(), [](const auto& c) {
            return !c.pending && !c.input;
        })) return true; // normal capture remains responsible; no speculative eviction
    // Per-sample DoVi metadata can change the color domain during CopySample.
    if (CComQIPtr<IMediaSideData> side = sample) {
        const BYTE* data = nullptr;
        size_t size = 0;
        if (SUCCEEDED(side->GetSideData(IID_MediaSideDataDOVIMetadataV2, &data, &size)) && size) return true;
    }
    CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &texture, &slice)) || !texture) return true;
    CComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return true;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != m_srcDXGIFormat || desc.MipLevels != 1 || slice >= desc.ArraySize ||
        desc.Width < m_srcWidth || desc.Height < m_srcHeight || desc.SampleDesc.Count != 1) return true;
    if (e.queryIssued) {
        if (m_pDeviceContext->GetData(e.completion, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
        e.queryIssued = false;
    }
    if (!e.completion) {
        const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        if (FAILED(m_pDevice->CreateQuery(&query, &e.completion))) return true;
    }
    const std::array<UINT, 5> key{UINT(m_srcDXGIFormat), UINT(m_srcWidth), UINT(m_srcHeight),
        UINT(m_srcExFmt.value), UINT(m_D3D11OutputFmt)};
    if (e.key != key) e.broken = false;
    if (e.broken) return true;
    if (e.key != key || !e.vp.IsReady()) {
        e.vp.ReleaseVideoDevice();
        e.key = key;
        DXGI_FORMAT output = m_D3D11OutputFmt;
        if (FAILED(e.vp.InitVideoDevice(m_pDevice, m_pDeviceContext, m_VendorId)) ||
            FAILED(e.vp.InitVideoProcessor(m_srcDXGIFormat, m_srcWidth, m_srcHeight,
                m_srcExFmt, DEINT_Disable, false, output)) || output != m_D3D11OutputFmt) {
            e.vp.ReleaseVideoDevice();
            e.broken = true;
            return true;
        }
    }
    const CRect native(0, 0, m_srcRect.Width(), m_srcRect.Height());
    // Borrow the ordinary native RGB scratch for the selected producer.
    // Only the existing budget-checked future slot may allocate a second.
    const bool sr = CanDlssSRUpscale(m_srcRect, ProcessingRect(), false);
    auto& normal = sr ? m_dlssSRSource : m_adaptiveTone.sourceRaw;
    // The current original may still own the ordinary RGB surface. A
    // second surface is necessary only while both originals coexist. Use
    // existing measured pool budget and current DXGI budget, not a new
    // timing/watermark constant or an allocation on each original.
    if (future && !normal.pTexture) {
        const uint64_t bytes = uint64_t(native.Width()) * uint64_t(native.Height()) * 4;
        const auto retained = GetCompletedPoolUsage(done).Retained();
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (!m_completedBudgetKnown || !m_completedAdapter || retained > m_completedByteLimit ||
            bytes > m_completedByteLimit - retained ||
            FAILED(m_completedAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
            memory.CurrentUsage > memory.Budget || bytes > memory.Budget - memory.CurrentUsage ||
            FAILED(normal.CheckCreate(m_pDevice, m_D3D11OutputFmt,
                native.Width(), native.Height(), Tex2D_DefaultShaderRTarget))) return true;
    }
    if (!normal.pTexture || !normal.pShaderResource || normal.type != Tex2D_DefaultShaderRTarget ||
        normal.desc.Format != m_D3D11OutputFmt ||
        normal.desc.Width != UINT(native.Width()) || normal.desc.Height != UINT(native.Height())) return true;
    if (ticket->Cancelled()) return true;
    e.srSource = sr;
    std::swap(e.rgb, normal);
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* empty = nullptr;
    m_pDeviceContext->PSSetShaderResources(0, 1, &empty);
    if (FAILED(e.vp.ProcessExternal(texture, slice, e.rgb.pTexture, m_srcRect, native))) {
        e.broken = true;
        ClearEarlyOriginal();
        return true;
    }
    // No extra decoder-owned COM reference survives this function. The
    // original is owned either by synchronous Receive or by its serialized
    // delivery queue. Only its independent RGB surface is retained here.
    if (ticket->Cancelled()) {
        ClearEarlyOriginal();
        return true;
    }
    PollSvdLutTemporalQueue();
    e.captured = QueueSvdLutTemporalFrame(e.rgb, native, pts, 0) != nullptr;
    if (e.captured) {
        ++e.captures;
        m_pDeviceContext->End(e.completion);
        e.queryIssued = true;
    }
    // Even on optional capture failure, reuse the successful CSC.
    e.sample = sample;
    e.decoderTexture = texture.p;
    e.decoderSlice = slice;
    e.pts = pts;
    e.epoch = epoch;
    e.rect = m_srcRect;
    e.sourceKey = key;
    e.ready = true;
    e.approved = e.consumed = false;
    e.fromFuture = future;
    ++e.prepared;
    if (future) ++e.futurePrepared;
    // PrepareFutureOriginal temporarily swaps the current image into the
    // future slot; preserve its existing AT sampling opportunity first.
    PrepareEarlyAdaptiveTone(e.rgb,native,pts,epoch,future ? m_futureOriginal.pts : INVALID_TIME);
    return true;
}

bool CDX11VideoProcessor::PrepareFutureOriginal(IMediaSample* sample, REFERENCE_TIME currentPts, uint64_t epoch)
{
    auto& e = m_earlyOriginal;
    if (!sample || m_futureOriginal.ready || !e.ready || !e.captured || e.pts != currentPts || e.epoch != epoch)
        return true;
    REFERENCE_TIME pts = INVALID_TIME, end = INVALID_TIME;
    if (sample->GetTime(&pts, &end) != S_OK || pts <= currentPts || end <= pts ||
        sample->IsDiscontinuity() == S_OK || sample->IsPreroll() == S_OK) return true;
    if (!EarlyOriginalDomainMatches(e) || !EarlyOriginalRoute(m_srcRect, ProcessingRect()) ||
        e.srSource != CanDlssSRUpscale(m_srcRect, ProcessingRect(), false)) return true;
    // Only one converter/query state, but separate RGB ownership and sample
    // identity. Restore current unconditionally, including early return.
    auto& current = static_cast<EarlyOriginalImage&>(e);
    std::swap(current, m_futureOriginal);
    struct Restore { EarlyOriginalImage& a; EarlyOriginalImage& b;
        ~Restore() { std::swap(a, b); } } restore{current, m_futureOriginal};
    return PrepareEarlyOriginal(sample, pts, epoch, true);
}

void CDX11VideoProcessor::ServiceFutureOriginal(uint64_t epoch)
{
    if (!m_futureOriginal.ready || m_futureOriginal.epoch != epoch) return;
    if (!EarlyOriginalDomainMatches(m_futureOriginal)) {
        DiscardEarlyOriginal(m_futureOriginal);
        m_futureOriginal = {};
        return;
    }
    auto& current = static_cast<EarlyOriginalImage&>(m_earlyOriginal);
    std::swap(current, m_futureOriginal);
    struct Restore { EarlyOriginalImage& a; EarlyOriginalImage& b;
        ~Restore() { std::swap(a, b); } } restore{current, m_futureOriginal};
    ServiceEarlyOriginal(current.pts, epoch);
}

void CDX11VideoProcessor::ActivateFutureOriginal(IMediaSample* sample, REFERENCE_TIME pts, uint64_t epoch)
{
    if (!m_futureOriginal.ready) return;
    if (!m_earlyOriginal.ready && m_futureOriginal.sample == sample && m_futureOriginal.pts == pts &&
        m_futureOriginal.epoch == epoch && EarlyOriginalDomainMatches(m_futureOriginal) &&
        !m_pFilter->m_playbackCancel.Capture()->Cancelled()) {
        std::swap(static_cast<EarlyOriginalImage&>(m_earlyOriginal), m_futureOriginal);
    } else {
        DiscardEarlyOriginal(m_futureOriginal);
        m_futureOriginal = {};
    }
}

void CDX11VideoProcessor::ServiceEarlyOriginal(REFERENCE_TIME pts, uint64_t epoch)
{
    auto& e = m_earlyOriginal;
    auto& b = m_svdLutRawTest;
    if (!EarlyOriginalDomainMatches(e) || !e.captured || e.pts != pts || e.epoch != epoch || !m_pDeviceContext ||
        m_pFilter->m_playbackCancel.Capture()->Cancelled() || m_completedPresentFairness.Active() ||
        CompletedGeometryChanging() || m_resizePending || !EarlyOriginalRoute(m_srcRect, ProcessingRect()) ||
        e.srSource != CanDlssSRUpscale(m_srcRect, ProcessingRect(), false)) return;
    // Drive the already queued thumbnail on actual GPU/worker readiness,
    // during existing Receive retries only. No Flush, new wait or timer.
    const bool waiting = std::any_of(b.captures.begin(), b.captures.end(), [pts](const auto& c) {
        return c.pts == pts && c.fence == 0 && (c.pending || bool(c.input));
    });
    if (!waiting || m_completedFrames.empty() || !m_completedBatches.empty()) return;
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream)) ||
        m_completedFrames.front().due - REFERENCE_TIME(stream) <=
            CompletedBackBufferLead(m_completedFrames.front(), CompletedSubmitLead())) return;
    if (e.queryIssued) {
        if (m_pDeviceContext->GetData(e.completion, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        e.queryIssued = false;
    }
    if (!b.model.ReadyForInput()) return;
    PollSvdLutTemporalQueue();
}

const Tex2D_t* CDX11VideoProcessor::ReuseEarlyOriginal(const CRect& source, const CRect& destination,
    REFERENCE_TIME pts, bool second, bool srConsumer)
{
    auto& e = m_earlyOriginal;
    if (!e.ready || !e.approved) return nullptr;
    if (second || pts != e.pts || source != e.rect || !EarlyOriginalDomainMatches(e) ||
        !m_completingSample ||
        !m_completedTarget || !m_completedTarget->videoOnly || !EarlyOriginalRoute(source, destination) ||
        e.srSource != srConsumer || e.srSource != CanDlssSRUpscale(source, destination, false) ||
        !e.rgb.pTexture || !e.rgb.pShaderResource) {
        FinishEarlyOriginal();
        return nullptr;
    }
    const auto held = m_D3D11VP.HoldFrame();
    if (held.pSample.p != e.sample || held.pTexture.p != e.decoderTexture || held.ArraySlice != e.decoderSlice) {
        FinishEarlyOriginal();
        return nullptr;
    }
    if (!e.consumed) {
        ++e.reused;
        if (e.fromFuture) ++e.futureReused;
        e.consumed = true;
    }
    return &e.rgb;
}
