// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Helper.h"
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"
#include "Mfg/CadenceTrace.h"
#include "Times.h"

namespace {
constexpr uint64_t MiB = 1024ull * 1024;
uint64_t TextureBytes(const D3D11_TEXTURE2D_DESC& desc)
{
    const auto pitch = (uint64_t(desc.Width) * 4 + 255) & ~255ull;
    return (pitch * desc.Height + 65535) & ~65535ull;
}
}

bool CDX11VideoProcessor::CanBufferCompletedSample() const
{
    return !m_completedFailed && !m_completedPresenter.Failed() && m_pDXGISwapChain1 &&
        m_pDevice && m_pDeviceContext && m_srcDXGIFormat == DXGI_FORMAT_NV12 &&
        m_SwapChainFmt == DXGI_FORMAT_B8G8R8A8_UNORM && !m_bInterlaced && !m_bDoubleFrames &&
        !SourceIsHDR() && !m_bVPUseRTXVideoHDR && !m_bVPRTXVideoHDR && !m_pPSHalfOUtoInterlace &&
        !DepthOverlayEnabled() && !m_showMfgMotion &&
        !m_videoRect.IsRectEmpty() && !m_windowRect.IsRectEmpty() &&
        (m_processingSize.cx > 0 || !m_renderRect.IsRectEmpty()) &&
        m_pFilter->m_pClock && !m_pFilter->m_bClockStalled;
}

bool CDX11VideoProcessor::CompletedFramesPending() const
{
    return !m_completedFrames.empty() || !m_completedBatches.empty();
}

bool CDX11VideoProcessor::LegacyFramesPending()
{
    std::lock_guard lock(m_mfgQueueLock);
    return m_mfgThread.Running() && !m_mfgThread.Failed() &&
        (m_mfgPreparedFrame || m_mfgQueue.PendingOriginals() || m_mfgLiveFlowGroups.PendingOriginals());
}

bool CDX11VideoProcessor::CanProduceCompletedFrame(REFERENCE_TIME due, std::optional<uint64_t>& deferredFrameFence)
{
    UpdateProcessingResolution();
    // Keep another generation in flight while the front batch waits for the GPU.
    // After-MFG upscale uses its own temporal history and a single-batch path.
    const size_t batchLimit = UpscaleAt(UpscaleSlot::BeforeMfg) && m_mfg.AsyncProducer() ? 2 : 1;
    if (m_completedBatches.size() >= batchLimit || m_completedFrames.size() >= 256) return false;
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return false;
    constexpr REFERENCE_TIME startupLead = UNITS / 4;
    const auto maxLead = m_extendedStartupPending ? startupLead : CompletedLead;
    if (due + m_rtVideoDelay - REFERENCE_TIME(stream) > maxLead) return false;
    if (m_completedFrames.empty()) return true;
    const auto& front = m_completedFrames.front();
    const auto remaining = front.due - REFERENCE_TIME(stream);
    if (remaining < CompletedSubmitLead() + 20'000) {
        // Give one queue head priority per input sample. After it leaves,
        // allow work in the submit margin while the next head's PTS is still ahead.
        if (!deferredFrameFence) deferredFrameFence = front.fence;
        if (front.fence == *deferredFrameFence || remaining <= 0) return false;
    }
    const auto completed = m_completedFence->GetCompletedValue();
    if (completed != UINT64_MAX && m_completedFenceValue - completed > 8) return false;
    uint64_t queued = 0;
    for (const auto& frame : m_completedFrames) queued += TextureBytes(frame.image.desc);
    for (const auto& batch : m_completedBatches) queued += TextureBytes(batch.target.image.desc);
    if (m_completedLast) queued += TextureBytes(m_completedLast->image.desc);
    const auto multiplier = (std::clamp)(m_mfgEffectiveMultiplier.load(std::memory_order_relaxed), 1u, 4u);
    auto nextDesc = m_completedFrames.back().image.desc;
    if (m_processingSize.cx > 0) {
        nextDesc.Width = m_processingSize.cx;
        nextDesc.Height = m_processingSize.cy;
    }
    const auto bytes = TextureBytes(nextDesc);
    const auto desired = (std::min)(bytes * multiplier, m_completedByteLimit / 3);
    return queued + (std::max)(bytes, desired) <= m_completedByteLimit;
}

HRESULT CDX11VideoProcessor::RenderCompletedSample(IMediaSample* sample)
{
    m_completingSample = true;
    struct Reset {
        CDX11VideoProcessor& owner;
        ~Reset() {
            owner.m_completingSample = false;
            owner.m_completedTarget.reset();
        }
    } reset{*this};
    const auto started = GetPreciseTick();
    const auto result = m_pFilter->DoRenderSample(sample);
    const auto finished = GetPreciseTick();
    m_RenderStats.paintticks = finished - started;
    if (mfg::SourceCadenceTrace().Enabled()) {
        REFERENCE_TIME pts = -1, end = -1;
        CRefTime stream;
        if (sample->GetTime(&pts, &end) == S_OK && SUCCEEDED(m_pFilter->StreamTime(stream))) {
            mfg::SourceCadenceTrace().Record("completed_produce", finished, pts, stream,
                0, mfg::MaxGenerated, 0, 0, 0, false, m_mfgLostGenerated, result, finished - started);
        }
    }
    return result;
}

bool CDX11VideoProcessor::PrepareCompletedTarget()
{
    if (!CanBufferCompletedSample()) return false;
    const bool videoOnly = m_processingSize.cx > 0;
    if (!videoOnly) ApplyPendingWindowResize();
    if (!m_completedFence) {
        CComQIPtr<ID3D11Device5> device(m_pDevice);
        CComQIPtr<ID3D11DeviceContext4> context(m_pDeviceContext);
        if (!device || !context || FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_completedFence)))) {
            m_completedFailed = true;
            return false;
        }
        m_completedContext = context;
        CComQIPtr<IDXGIDevice> dxgi(m_pDevice);
        CComPtr<IDXGIAdapter> adapter;
        if (dxgi && SUCCEEDED(dxgi->GetAdapter(&adapter))) adapter.QueryInterface(&m_completedAdapter);
    }
    D3D11_TEXTURE2D_DESC desc{};
    CompletedFrame target;
    target.videoOnly = videoOnly;
    if (videoOnly) {
        target.picture = ProcessingRect();
        desc = CreateTex2DDesc(m_SwapChainFmt, target.picture.Width(), target.picture.Height(),
                               Tex2D_DefaultShaderRTarget);
        if (FAILED(m_processingPostScale.CheckCreate(m_pDevice, m_InternalTexFmt,
                desc.Width, desc.Height, GetPostScaleSteps()))) return false;
    } else {
        CComPtr<ID3D11Texture2D> back;
        if (FAILED(m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        back->GetDesc(&desc);
        target.picture = m_videoRect;
        const CRect bounds(0, 0, desc.Width, desc.Height);
        CRect visible;
        visible.IntersectRect(target.picture, bounds);
        if (visible != target.picture) {
            const double scale = (std::min)({1.0, double(desc.Width) / target.picture.Width(),
                                           double(desc.Height) / target.picture.Height()});
            target.picture.SetRect(0, 0, (std::max)(1L, LONG(std::lround(target.picture.Width() * scale))),
                                  (std::max)(1L, LONG(std::lround(target.picture.Height() * scale))));
        }
    }
    if (!AcquireCompletedTarget(desc, target)) return false;
    try {
        m_completedPresenter.Start([this](int64_t& deadline) { return NextCompletedFrame(deadline); },
                                   [this] { return PresentCompletedFrame(); });
    } catch (...) {
        m_completedFailed = true;
        return false;
    }
    m_completedTarget.emplace(std::move(target));
    return true;
}

bool CDX11VideoProcessor::AcquireCompletedTarget(const D3D11_TEXTURE2D_DESC& desc, CompletedFrame& target)
{
    const auto bytes = TextureBytes(desc);
    const auto completed = m_completedFence->GetCompletedValue();
    if (completed == UINT64_MAX) return false;
    uint64_t inUse = 0;
    for (const auto& frame : m_completedFrames) inUse += TextureBytes(frame.image.desc);
    for (const auto& batch : m_completedBatches) inUse += TextureBytes(batch.target.image.desc);
    if (m_completedLast) inUse += TextureBytes(m_completedLast->image.desc);
    if (m_completedTarget) inUse += TextureBytes(m_completedTarget->image.desc);
    const auto tick = GetTickCount64();
    if (!m_completedBudgetTick || tick - m_completedBudgetTick >= 250) {
        m_completedBudgetTick = tick;
        m_completedByteLimit = 256 * MiB;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (m_completedAdapter && SUCCEEDED(m_completedAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            auto allocated = inUse;
            for (const auto& frame : m_completedFree) allocated += TextureBytes(frame.image.desc);
            const auto headroom = memory.Budget > memory.CurrentUsage + 256 * MiB
                ? memory.Budget - memory.CurrentUsage - 256 * MiB : 0;
            m_completedByteLimit = (std::min)({4096 * MiB, memory.Budget / 2, allocated + headroom / 2});
        }
    }
    if (inUse + bytes > m_completedByteLimit && (m_completedTarget || !m_completedBatches.empty())) return false;
    for (auto it = m_completedFree.begin(); it != m_completedFree.end();) {
        const auto& old = it->image.desc;
        if (old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format && it->fence <= completed) {
            target.image = std::move(it->image);
            m_completedFree.erase(it);
            return true;
        }
        // Keep in-flight surfaces available for a later frame, after the GPU
        // has finished reading them. Discarding them forces extra allocations.
        if (it->fence > completed) ++it;
        else it = m_completedFree.erase(it);
    }
    return SUCCEEDED(target.image.Create(m_pDevice, desc.Format, desc.Width, desc.Height, Tex2D_DefaultShaderRTarget));
}

void CDX11VideoProcessor::RecycleCompletedFrame(CompletedFrame&& frame)
{
    if (m_completedFree.size() < 4 && TextureBytes(frame.image.desc) * 4 < m_completedByteLimit)
        m_completedFree.push_back(std::move(frame));
}

bool CDX11VideoProcessor::QueueCompletedFrame(CompletedFrame&& frame)
{
    frame.fence = ++m_completedFenceValue;
    if (FAILED(m_completedContext->Signal(m_completedFence, frame.fence))) return false;
    if (mfg::SourceCadenceTrace().Enabled()) {
        mfg::SourceCadenceTrace().Record("completed_enqueue", GetPreciseTick(), frame.pts, -1,
            frame.serial, frame.phase, 0, 0, 0, true, m_mfgLostGenerated, S_OK, 0, frame.due);
    }
    m_completedFrames.push_back(std::move(frame));
    m_completedPresenter.Wake();
    return true;
}

bool CDX11VideoProcessor::PrepareCompletedBatch(REFERENCE_TIME stream)
{
    auto& batch = m_completedBatches.front();
    if (batch.phases.empty()) {
        const bool queued = QueueCompletedFrame(std::move(batch.target));
        m_completedBatches.pop_front();
        m_pDeviceContext->Flush();
        if (!queued) m_completedFailed = true;
        return true;
    }
    const auto& original = batch.phases.back();
    const auto& first = batch.phases.front();
    if (first.index != mfg::MaxGenerated && first.presentationTime > stream &&
        m_mfg.Frame(first.serial, first.index, original.timestamp).pending) return false;
    const auto started = GetPreciseTick();
    // Use the geometry and input layout recorded for this batch.
    const auto picture = batch.target.picture;
    if (!batch.target.videoOnly && picture != m_videoRect && mfg::SourceCadenceTrace().Enabled()) {
        mfg::SourceCadenceTrace().Record("completed_geometry_transition", GetPreciseTick(),
            original.timestamp, stream, original.serial, original.index,
            0, 0, 0, false, m_mfgLostGenerated, S_OK);
    }
    bool sampled = false;
    for (const auto& phase : batch.phases) {
        if (phase.index == mfg::MaxGenerated) break;
        const auto surface = m_mfg.Frame(phase.serial, phase.index, original.timestamp);
        CompletedFrame target;
        const char* drop = nullptr;
        if (phase.presentationTime <= stream) {
            drop = surface.pending ? "completed_drop_ngx_late" : "completed_drop_compose_late";
        } else if (!surface.texture) {
            drop = "completed_drop_surface";
        } else if (!m_completedFrames.empty() && phase.presentationTime <= m_completedFrames.back().due) {
            drop = "completed_drop_overlap";
        } else if (!AcquireCompletedTarget(batch.target.image.desc, target)) {
            drop = "completed_drop_texture";
        }
        if (drop) {
            ++m_mfgLostGenerated;
            ++m_mfgSkipped;
            if (mfg::SourceCadenceTrace().Enabled()) {
                mfg::SourceCadenceTrace().Record(drop, GetPreciseTick(), phase.timestamp, stream,
                    phase.serial, phase.index, 0, 0, 0, false, m_mfgLostGenerated, S_FALSE);
            }
            continue;
        }
        target.picture = picture;
        target.videoOnly = batch.target.videoOnly;
        target.pts = phase.timestamp;
        target.due = phase.presentationTime;
        target.serial = phase.serial;
        target.phase = phase.index;
        target.mfg = true;
        sampled = true;
        if (FAILED(RenderMfgVideo(surface, phase.nativeInput, target.image.pTexture, phase.timestamp,
                                  picture, batch.videoOnlyInput)) ||
            !QueueCompletedFrame(std::move(target))) {
            ++m_mfgLostGenerated;
            ++m_mfgSkipped;
        }
    }
    if (sampled) m_mfg.MarkConsumed(original.serial);
    const auto serial = original.serial;
    const auto pts = original.timestamp;
    auto& target = batch.target;
    target.due = original.presentationTime;
    target.serial = original.serial;
    target.mfg = true;
    const mfg::FrameSurface surface{original.original->texture.Get(), original.original->view.Get()};
    auto rendered = RenderMfgVideo(surface, original.nativeInput, target.image.pTexture, original.timestamp,
                                   picture, batch.videoOnlyInput);
    if (FAILED(rendered)) {
        rendered = RenderMfgVideo(surface, original.nativeInput, target.image.pTexture, original.timestamp,
                                  picture, batch.videoOnlyInput, true);
    }
    if (FAILED(rendered) || !QueueCompletedFrame(std::move(target))) {
        ++m_mfgLostReal;
        ++m_mfgSkipped;
        m_completedFailed = true;
    }
    m_completedBatches.pop_front();
    m_pDeviceContext->Flush();
    if (mfg::SourceCadenceTrace().Enabled()) {
        const auto finished = GetPreciseTick();
        mfg::SourceCadenceTrace().Record("completed_compose", finished, pts, stream,
            serial, mfg::MaxGenerated, 0, 0, 0, true, m_mfgLostGenerated, rendered, finished - started);
    }
    return true;
}

bool CDX11VideoProcessor::NextCompletedFrame(int64_t& deadline)
{
    if (m_pFilter->m_completedEndOfStream) m_pFilter->TryCompleteBufferedEndOfStream();
    deadline = int64_t(GetPreciseTick());
    if (!m_pFilter->m_RendererLock.TryLock()) {
        deadline += int64_t(GetPreciseTicksPerSecond() / 1000);
        return true;
    }
    struct Unlock { CCritSec& lock; ~Unlock() { lock.Unlock(); } } unlock{m_pFilter->m_RendererLock};
    if (m_pFilter->m_filterState != State_Running || m_pFilter->m_bFlushing || !m_pDXGISwapChain1)
        return false;
    if (!m_completedBatches.empty()) return true;
    if (m_completedFrames.empty()) {
        if (!m_pFilter->m_completedEndOfStream) return false;
        deadline += int64_t(GetPreciseTicksPerSecond() / 1000);
        return true;
    }
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return false;
    const auto delta = m_completedFrames.front().due - REFERENCE_TIME(stream) - CompletedSubmitLead();
    deadline += int64_t((std::clamp)(delta, REFERENCE_TIME(0), REFERENCE_TIME(200'000)) * GetPreciseTicksPerSecond() / UNITS);
    return true;
}

REFERENCE_TIME CDX11VideoProcessor::CompletedSubmitLead()
{
    const auto refresh = m_dRefreshPeriodMs > 0.0 ? 1000.0 / m_dRefreshPeriodMs : 0.0;
    // Window/compositor transitions are not representative of steady display
    // submission latency. If they train DisplaySubmitPacing, a short resize can
    // raise its p95 lead from the normal ~1.5 ms to the 4 ms cap for roughly a
    // full 96-sample window. That shortens the gap available for composing
    // pending MFG batches and can starve the completed queue until it empties.
    // Keep resetting the estimator for a brief settling period after the last
    // geometry event; Start() then uses the normal initial lead.
    constexpr ULONGLONG geometrySettleMs = 500;
    const auto now = GetTickCount64();
    if (m_resizeLastChangeTick && now >= m_resizeLastChangeTick &&
        now - m_resizeLastChangeTick < geometrySettleMs) {
        m_completedDisplayPacing.Reset();
    }
    return REFERENCE_TIME(m_completedDisplayPacing.Start(refresh) * 10'000);
}

bool CDX11VideoProcessor::PresentCompletedFrame()
{
    if (!m_pFilter->m_RendererLock.TryLock()) return false;
    struct Unlock { CCritSec& lock; ~Unlock() { lock.Unlock(); } } unlock{m_pFilter->m_RendererLock};
    if (m_pFilter->m_filterState != State_Running || m_pFilter->m_bFlushing || !m_pDXGISwapChain1) return true;
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return false;
    if (m_completedFrames.empty() || m_completedFrames.front().due > REFERENCE_TIME(stream) + CompletedSubmitLead())
        return m_completedBatches.empty() || PrepareCompletedBatch(stream);
    const auto completed = m_completedFence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        ClearCompletedFrames(false);
        m_completedFailed = true;
        return true;
    }
    if (completed < m_completedFrames.front().fence) return false;
    auto frame = std::move(m_completedFrames.front());
    m_completedFrames.pop_front();
    if (frame.phase != mfg::MaxGenerated && !m_completedFrames.empty() &&
        m_completedFrames.front().due <= REFERENCE_TIME(stream)) {
        ++m_mfgLostGenerated;
        ++m_mfgSkipped;
        if (mfg::SourceCadenceTrace().Enabled()) {
            mfg::SourceCadenceTrace().Record("completed_drop_present_late", GetPreciseTick(), frame.pts, stream,
                frame.serial, frame.phase, 0, 0, 0, true, m_mfgLostGenerated, S_FALSE);
        }
        RecycleCompletedFrame(std::move(frame));
        return true;
    }
    m_RenderStats.syncoffset = REFERENCE_TIME(stream) - frame.due;
    const auto refresh = m_dRefreshPeriodMs > 0.0 ? 1000.0 / m_dRefreshPeriodMs : 0.0;
    constexpr ULONGLONG geometrySettleMs = 500;
    const auto nowTick = GetTickCount64();
    const bool geometryTransient = m_resizeLastChangeTick && nowTick >= m_resizeLastChangeTick &&
        nowTick - m_resizeLastChangeTick < geometrySettleMs;
    if (geometryTransient) {
        // Resize/compositor lateness is transient and must not consume the
        // steady-state batch-composition window after the user stops resizing.
        m_completedDisplayPacing.Reset();
    } else {
        m_completedDisplayPacing.Observe(m_RenderStats.syncoffset / 10'000.0, refresh);
    }
    const auto started = GetPreciseTick();
    auto hr = PresentCompletedVideo(frame, true);
    if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen && SUCCEEDED(InitSwapChain(false))) {
        hr = PresentCompletedVideo(frame, true);
    }
    m_RenderStats.presentticks = GetPreciseTick() - started;
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        // Present was deliberately non-blocking. Keep ownership of this frame
        // and release RendererLock so the source/producer can make progress.
        // The presentation worker retries after ~1 ms. Generated frames may
        // still be discarded later if they truly miss their display slot; an
        // original frame is never discarded by this retry path.
        m_completedFrames.push_front(std::move(frame));
        return false;
    }
    if (hr == S_OK && frame.phase == mfg::MaxGenerated) {
        m_pFilter->ReportCompletedMediaTime(frame.pts, frame.end);
        m_pFilter->m_DrawStats.Add(GetPreciseTick());
    }
    if (hr == S_OK) {
        if (frame.mfg) {
            if (frame.phase == mfg::MaxGenerated) ++m_mfgRealPresents;
            else ++m_mfgGeneratedPresents;
        } else if (m_mfgMultiplier.load(std::memory_order_relaxed) >= 2) {
            ++m_mfgFallbackPresents;
        }
    }
    if (mfg::SourceCadenceTrace().Enabled()) {
        const auto now = int64_t(GetPreciseTick());
        const auto due = now + int64_t((frame.due - REFERENCE_TIME(stream)) * GetPreciseTicksPerSecond() / UNITS);
        mfg::SourceCadenceTrace().Record("completed_present", now, frame.pts, stream,
            frame.serial, frame.phase, 0, 0, due, true, m_mfgLostGenerated, hr, m_RenderStats.presentticks);
    }
    const auto readFence = ++m_completedFenceValue;
    if (SUCCEEDED(m_completedContext->Signal(m_completedFence, readFence)) && SUCCEEDED(hr)) {
        frame.fence = readFence;
        if (m_completedLast) RecycleCompletedFrame(std::move(*m_completedLast));
        m_completedLast = std::move(frame);
    }
    return true;
}

HRESULT CDX11VideoProcessor::DrawCompletedVideo(const CompletedFrame& frame, ID3D11Texture2D* target, const CRect& picture)
{
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    const auto& source = frame.image.desc;
    if (frame.picture == picture && source.Width == desc.Width && source.Height == desc.Height && source.Format == desc.Format) {
        m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        m_pDeviceContext->CopyResource(target, frame.image.pTexture);
        return S_OK;
    }
    CRect from;
    if (!from.IntersectRect(frame.picture, CRect(0, 0, source.Width, source.Height)) || picture.IsRectEmpty()) return E_INVALIDARG;
    const CRect to(
        picture.left + MulDiv(from.left - frame.picture.left, picture.Width(), frame.picture.Width()),
        picture.top + MulDiv(from.top - frame.picture.top, picture.Height(), frame.picture.Height()),
        picture.left + MulDiv(from.right - frame.picture.left, picture.Width(), frame.picture.Width()),
        picture.top + MulDiv(from.bottom - frame.picture.top, picture.Height(), frame.picture.Height()));
    CComPtr<ID3D11RenderTargetView> view;
    auto hr = m_pDevice->CreateRenderTargetView(target, nullptr, &view);
    if (FAILED(hr)) return hr;
    const float black[4] = {0, 0, 0, 1};
    m_pDeviceContext->ClearRenderTargetView(view, black);
    const CRect bounds(0, 0, desc.Width, desc.Height);
    CRect visible;
    if (frame.videoOnly && visible.IntersectRect(to, bounds) && visible == to) {
        const auto scaled = ResizeShaderPass(frame.image, target, from, to, 0, false, &m_completedResizeTexture);
        if (SUCCEEDED(scaled)) return scaled;
    }
    return TextureCopyRect(frame.image, target, from, to, m_pPS_Simple, nullptr, 0, false, true);
}

HRESULT CDX11VideoProcessor::PresentCompletedVideo(const CompletedFrame& frame, bool scheduled)
{
    // Completed video is independent of the swap-chain size. Keep continuous
    // window changes coalesced instead of forcing ResizeBuffers on every
    // presentation. While the final size is still settling, remap the current
    // destination rectangle into the last committed back buffer.
    ApplyPendingWindowResize();
    const bool trace = scheduled && mfg::SourceCadenceTrace().Enabled();
    const auto started = trace ? GetPreciseTick() : 0;
    CComPtr<ID3D11Texture2D> back;
    auto hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) return hr;
    CRect picture = m_videoRect;
    if (m_resizePending && !m_resizeTarget.IsRectEmpty()) {
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        const auto requested = m_resizeTarget.Size();
        if (requested.cx > 0 && requested.cy > 0) {
            const bool flip = m_UsedSwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                              m_UsedSwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            if (flip) {
                // DXGI_SCALING_NONE exposes only the client-sized portion of
                // the old flip buffer. Fit the requested geometry into that
                // visible area until the deferred ResizeBuffers is committed.
                CRect client;
                if (!GetClientRect(m_hWnd, &client) || client.IsRectEmpty())
                    client.SetRect(0, 0, requested.cx, requested.cy);
                const LONG width = (std::min<LONG>)(static_cast<LONG>(desc.Width), client.Width());
                const LONG height = (std::min<LONG>)(static_cast<LONG>(desc.Height), client.Height());
                const double scale = (std::min)(double(width) / requested.cx, double(height) / requested.cy);
                const auto x = [&](LONG value) {
                    return LONG(std::lround(width * 0.5 + (value - requested.cx * 0.5) * scale));
                };
                const auto y = [&](LONG value) {
                    return LONG(std::lround(height * 0.5 + (value - requested.cy * 0.5) * scale));
                };
                picture.SetRect(x(picture.left), y(picture.top), x(picture.right), y(picture.bottom));
            } else {
                // Blt-model swap chains stretch the old buffer to the new
                // client area, so draw in the corresponding old-buffer space.
                picture.SetRect(MulDiv(picture.left, desc.Width, requested.cx),
                                MulDiv(picture.top, desc.Height, requested.cy),
                                MulDiv(picture.right, desc.Width, requested.cx),
                                MulDiv(picture.bottom, desc.Height, requested.cy));
            }
        }
    }
    hr = DrawCompletedVideo(frame, back, picture);
    if (trace) {
        const auto finished = GetPreciseTick();
        mfg::SourceCadenceTrace().Record("completed_blit", finished, frame.pts, -1,
            frame.serial, frame.phase, 0, 0, 0, true, m_mfgLostGenerated, hr, finished - started);
    }
    if (FAILED(hr)) return hr;
    if (m_bVBlankBeforePresent && m_pDXGIOutput) m_pDXGIOutput->WaitForVBlank();
    return FinishMfgPresent(back, frame.pts, 0, scheduled);
}

void CDX11VideoProcessor::ClearCompletedFrames(bool release)
{
    if (release) m_completedPresenter.Stop();
    m_completedFrames.clear();
    m_completedFree.clear();
    m_completedLast.reset();
    m_completedBatches.clear();
    m_completedTarget.reset();
    m_completedGroups.Clear();
    m_completedDisplayPacing.Reset();
    m_completedPresenter.Wake();
    if (release) {
        m_completedResizeTexture.Release();
        m_completedContext.Release();
        m_completedFence.Release();
        m_completedAdapter.Release();
        m_completedFenceValue = m_completedByteLimit = m_completedBudgetTick = 0;
        m_completedFailed = false;
    }
}

CDX11VideoProcessor::CompletedBufferTime CDX11VideoProcessor::GetCompletedBufferTime(REFERENCE_TIME stream) const
{
    CompletedBufferTime time;
    if (!m_completedFence || m_completedFrames.empty()) return time;
    const auto fence = m_completedFence->GetCompletedValue();
    if (fence == UINT64_MAX) return time;
    bool ready = true;
    std::optional<REFERENCE_TIME> previousDue;
    for (const auto& frame : m_completedFrames) {
        time.queued = (std::max)(time.queued, frame.due - stream);
        const bool increasing = !previousDue || frame.due > *previousDue;
        if (!increasing) ++time.nonIncreasing;
        ready = ready && increasing && frame.fence <= fence;
        if (ready) time.ready = (std::max)(time.ready, frame.due - stream);
        previousDue = frame.due;
    }
    return time;
}

std::wstring CDX11VideoProcessor::CompletedBufferInfo()
{
    CRefTime stream;
    if (!m_completedFence || FAILED(m_pFilter->StreamTime(stream))) return {};

    uint64_t bytes = 0;
    unsigned width = 0, height = 0;
    const auto add = [&](const CompletedFrame& frame) {
        bytes += TextureBytes(frame.image.desc);
    };
    for (const auto& frame : m_completedFrames) add(frame);
    for (const auto& batch : m_completedBatches) add(batch.target);
    for (const auto& frame : m_completedFree) add(frame);
    if (m_completedTarget) add(*m_completedTarget);
    if (m_completedLast) add(*m_completedLast);
    if (!m_completedFrames.empty()) {
        width = m_completedFrames.back().image.desc.Width;
        height = m_completedFrames.back().image.desc.Height;
    }
    const auto time = GetCompletedBufferTime(stream);
    auto info = std::format(L"\nCompleted video buffer: {} ms, {} queued, {}x{}, {:.1f} MiB estimated pool, {:.1f} MiB pool limit, {} ms queued ahead",
        time.ready / 10000, m_completedFrames.size(), width, height, double(bytes) / MiB,
        double(m_completedByteLimit) / MiB, time.queued / 10000);
    if (time.nonIncreasing) info += std::format(L", {} deadline reversals or ties", time.nonIncreasing);
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (m_completedAdapter && SUCCEEDED(m_completedAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
        info += std::format(L", {:.1f} MiB process local usage, {:.1f} MiB process local budget",
            double(memory.CurrentUsage) / MiB, double(memory.Budget) / MiB);
    }
    return info;
}
