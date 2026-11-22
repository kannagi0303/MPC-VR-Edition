// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Helper.h"
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"
#include "Times.h"
#include "Mfg/CadenceTrace.h"
#include <cstring>
#include <array>
#include <span>

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
        !(m_mfgMultiplier && MfgSourceMeetsDisplayRate()) &&
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

const char* CDX11VideoProcessor::CompletedAdmissionBlock(REFERENCE_TIME due, std::optional<uint64_t>& deferredFrameFence)
{
    // A bank choice must belong only to this one admission attempt.
    m_mfg.ClearNextBankReservation();
    UpdateProcessingResolution();
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return "completed_wait_clock";
    const auto completed = m_completedFence ? m_completedFence->GetCompletedValue() : 0;
    if (m_completedFence) RefreshCompletedMemoryBudget(completed);
    if (m_completedFrames.size() >= 256) return "completed_wait_queue_limit";
    const auto maxLead = m_extendedStartupPending ? CompletedStartupLead : CompletedLead;
    if (due + m_rtVideoDelay - REFERENCE_TIME(stream) > maxLead) return "completed_wait_lookahead";
    const bool hasCompletedFrame = !m_completedFrames.empty();
    if (!hasCompletedFrame && m_completedBatches.empty()) return nullptr;
    const auto margin = CompletedSubmitLead() + 20'000;
    if (hasCompletedFrame) {
        const auto& front = m_completedFrames.front();
        const auto remaining = front.due - REFERENCE_TIME(stream);
        if (remaining < margin) {
            // Give one queue head priority per input sample. After it leaves,
            // allow work in the submit margin while the next head's PTS is still ahead.
            if (!deferredFrameFence) deferredFrameFence = front.fence;
            if (front.fence == *deferredFrameFence || remaining <= 0) return "completed_wait_present_deadline";
        }
    }
    if (completed == UINT64_MAX) return "completed_wait_device_removed";
    if (m_completedFenceValue - completed > 8) return "completed_wait_gpu_fence";
    const auto committed = GetCompletedPoolUsage(completed).Committed();
    auto nextDesc = (hasCompletedFrame ? m_completedFrames.back() : m_completedBatches.back().target).image.desc;
    if (m_processingSize.cx > 0) {
        nextDesc.Width = m_processingSize.cx;
        nextDesc.Height = m_processingSize.cy;
    }
    const auto bytes = TextureBytes(nextDesc);
    if (!m_completedBatches.empty()) {
        const char* block = CompletedOverlapBlock(stream, margin, completed, committed, bytes);
        if (block && m_completedResizeRecovery.load(std::memory_order_relaxed) && m_mfg.AsyncProducer() &&
            (std::strcmp(block, "completed_wait_mfg_slots") == 0 ||
             std::strcmp(block, "completed_wait_mfg_bank") == 0)) {
            // B08 is a resize-recovery escape only. Seek/flush resets the arm,
            // so seek prebuffering follows the GitHub stable bank/pending path.
            // Do not decide MFG startup from the Ready-buffer percentage:
            // seek naturally starts with zero Ready and must still generate.
            // A recyclable NGX bank is an optional enhancement resource, not
            // ownership of the original frame. When bank pressure prevents
            // receiving another source, retire the oldest buffered batch now.
            // Ready synthetic phases are still rendered; unfinished ones are
            // discarded, while the independent original snapshot is enqueued.
            // Without this escape, a pending generated phase can block the
            // entire batch until the ORIGINAL's deadline, and the queue can
            // drain even though source/GPU processing has sufficient capacity.
            PrepareCompletedBatch(stream, true);
            // Re-check GPU completion, capacity, and bank ownership after
            // rendering any ready phases and enqueuing the protected original.
            // A forced batch is not permission to exceed the fence budget.
            if (m_completedFailed) return "completed_wait_prepare_failed";
            if (m_completedFrames.size() >= 256) return "completed_wait_queue_limit";
            const auto latestCompleted = m_completedFence->GetCompletedValue();
            if (latestCompleted == UINT64_MAX) return "completed_wait_device_removed";
            if (m_completedFenceValue - latestCompleted > 8) return "completed_wait_gpu_fence";
            block = m_completedBatches.empty() ? nullptr :
                CompletedOverlapBlock(stream, margin, latestCompleted,
                    GetCompletedPoolUsage(latestCompleted).Committed(), bytes);
        }
        if (block) return block;
        if (!m_completedBatches.empty()) {
            auto& trace = mfg::SourceCadenceTrace();
            if (trace.Enabled()) {
                trace.Record("completed_mfg_overlap_admit", GetPreciseTick(), due, stream,
                    m_completedBatches.front().phases.front().serial, 0, 0, 0, 0, false, m_mfgLostGenerated, S_OK);
            }
            return nullptr;
        }
    }
    const auto multiplier = (std::clamp)(m_mfgEffectiveMultiplier.load(std::memory_order_relaxed), 1u, 4u);
    const auto desired = (std::min)(bytes * multiplier, m_completedByteLimit / 3);
    return GetCompletedPoolUsage(completed).Committed() + (std::max)(bytes, desired) <= m_completedByteLimit
        ? nullptr : "completed_wait_budget";
}

const char* CDX11VideoProcessor::CompletedOverlapBlock(REFERENCE_TIME stream, REFERENCE_TIME margin, uint64_t completed,
                                                       uint64_t committed, uint64_t nextFrameBytes) const
{
    if (!m_completedBudgetKnown) return "completed_wait_budget_unknown";
    if (m_extendedStartupPending || CompletedGeometryChanging() || !m_mfgMultiplier || m_mfgOriginalPrime)
        return "completed_wait_batch";
    const bool beforeMfgSecondBatch = m_completedBatches.size() == 1 &&
        UpscaleAt(UpscaleSlot::BeforeMfg) && m_mfg.AsyncProducer();
    const size_t reservedBanks = beforeMfgSecondBatch ? 0 : 1;
    if (m_completedBatches.size() + reservedBanks >= m_mfg.BankCount()) return "completed_wait_mfg_slots";
    const auto& phases = m_completedBatches.front().phases;
    if (phases.empty()) return "completed_wait_compose";
    // Keep the two-batch before-MFG path available even while the previous
    // batch is ready to compose. Keep the stricter deadline and fence checks
    // for other overlap cases.
    if (!beforeMfgSecondBatch) {
        if (m_completedFrames.empty()) return "completed_wait_batch";
        const auto& frame = m_completedFrames.front();
        if (frame.fence > completed) return "completed_wait_gpu_fence";
        if (frame.due - stream <= margin) return "completed_wait_present_deadline";
        if (phases.front().index == mfg::MaxGenerated || !m_mfg.GenerationPending(phases.front().serial))
            return "completed_wait_compose";
        if (phases.front().presentationTime - stream <= margin) return "completed_wait_mfg_deadline";
    }
    // All still-buffered generated groups own their bank outputs until
    // composed or retired. Do not reserve one of those banks merely because
    // its D3D12 producer fence has already completed.
    std::array<uint64_t, 4> protectedGroups{};
    size_t protectedCount = 0;
    for (const auto& batch : m_completedBatches) {
        if (batch.phases.empty()) return "completed_wait_compose";
        if (protectedCount == protectedGroups.size()) return "completed_wait_mfg_slots";
        protectedGroups[protectedCount++] = batch.phases.front().serial;
    }
    const auto bank = m_mfg.ReserveNextBankSerial(!beforeMfgSecondBatch,
        std::span<const uint64_t>(protectedGroups.data(), protectedCount));
    if (!bank) return "completed_wait_mfg_bank";
    const auto reserved = PendingGeneratedBytes() + nextFrameBytes * m_mfg.MaxAvailableMultiplier();
    return committed + reserved <= m_completedByteLimit ? nullptr : "completed_wait_budget";
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

HRESULT CDX11VideoProcessor::PrepareCompletedTarget()
{
    if (!CanBufferCompletedSample()) return E_FAIL;
    const bool videoOnly = m_processingSize.cx > 0;
    if (!videoOnly) ApplyPendingWindowResize();
    if (!m_completedFence) {
        CComQIPtr<ID3D11Device5> device(m_pDevice);
        CComQIPtr<ID3D11DeviceContext4> context(m_pDeviceContext);
        if (!device || !context || FAILED(device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_completedFence)))) {
            m_completedFailed = true;
            return E_FAIL;
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
        const auto hr = m_processingPostScale.CheckCreate(m_pDevice, m_InternalTexFmt,
            desc.Width, desc.Height, GetPostScaleSteps());
        if (FAILED(hr)) return hr;
    } else {
        CComPtr<ID3D11Texture2D> back;
        const auto hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hr)) return hr;
        back->GetDesc(&desc);
        target.picture = PictureForPlayback(CanProjectJvr());
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
    const auto acquired = AcquireCompletedTarget(desc, target);
    if (acquired != S_OK) return acquired;
    try {
        m_completedPresenter.Start([this](int64_t& deadline) { return NextCompletedFrame(deadline); },
                                   [this] { return PresentCompletedFrame(); });
    } catch (...) {
        m_completedFailed = true;
        return E_FAIL;
    }
    m_completedTarget.emplace(std::move(target));
    return S_OK;
}

CDX11VideoProcessor::CompletedPoolUsage CDX11VideoProcessor::GetCompletedPoolUsage(uint64_t completed) const
{
    CompletedPoolUsage usage;
    for (const auto& frame : m_completedFrames) usage.active += TextureBytes(frame.image.desc);
    for (const auto& batch : m_completedBatches) usage.active += TextureBytes(batch.target.image.desc);
    if (m_completedLast) usage.active += TextureBytes(m_completedLast->image.desc);
    if (m_completedTarget) usage.active += TextureBytes(m_completedTarget->image.desc);
    for (const auto& frame : m_completedFree) {
        auto& bytes = completed != UINT64_MAX && frame.fence <= completed ? usage.reusable : usage.gpuPending;
        bytes += TextureBytes(frame.image.desc);
    }
    return usage;
}

uint64_t CDX11VideoProcessor::PendingGeneratedBytes() const
{
    uint64_t reserved = 0;
    for (const auto& batch : m_completedBatches) {
        // The final phase's original target is already included in active usage.
        if (!batch.phases.empty()) reserved += TextureBytes(batch.target.image.desc) * (batch.phases.size() - 1);
    }
    return reserved;
}

void CDX11VideoProcessor::RefreshCompletedMemoryBudget(uint64_t completed)
{
    const auto tick = GetTickCount64();
    if (completed == UINT64_MAX || (m_completedBudgetTick && tick - m_completedBudgetTick < 250)) return;
    m_completedBudgetTick = tick;
    m_completedByteLimit = 256 * MiB;
    auto& memory = m_completedMemoryInfo;
    m_completedBudgetKnown = m_completedAdapter &&
        SUCCEEDED(m_completedAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
    if (!m_completedBudgetKnown) return;

    auto allocated = GetCompletedPoolUsage(completed).Retained();
    constexpr uint64_t reserve = 256 * MiB;
    const auto available = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
    const auto headroom = available > reserve ? available - reserve : 0;
    const auto usable = memory.Budget > reserve ? memory.Budget - reserve : 0;
    const auto pressure = memory.CurrentUsage > usable ? memory.CurrentUsage - usable : 0;
    m_completedByteLimit = (std::min)({4096 * MiB, memory.Budget / 2,
        allocated - (std::min)(allocated, pressure) + headroom / 2});
    for (auto it = m_completedFree.begin(); allocated > m_completedByteLimit && it != m_completedFree.end();) {
        if (it->fence > completed) {
            ++it;
            continue;
        }
        allocated -= TextureBytes(it->image.desc);
        it = m_completedFree.erase(it);
    }
}

HRESULT CDX11VideoProcessor::AcquireCompletedTarget(const D3D11_TEXTURE2D_DESC& desc, CompletedFrame& target)
{
    const auto bytes = TextureBytes(desc);
    const auto completed = m_completedFence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    RefreshCompletedMemoryBudget(completed);
    const auto committed = GetCompletedPoolUsage(completed).Committed();
    if (committed + bytes > m_completedByteLimit && (m_completedTarget || !m_completedBatches.empty())) return S_FALSE;
    // One output costs one slot. Matching reclaimable textures are moved, and
    // incompatible reclaimable textures are released before allocating another.
    for (auto it = m_completedFree.begin(); it != m_completedFree.end();) {
        const auto& old = it->image.desc;
        if (old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format && it->fence <= completed) {
            target.image = std::move(it->image);
            m_completedFree.erase(it);
            return S_OK;
        }
        // Keep in-flight surfaces available for a later frame, after the GPU
        // has finished reading them. Discarding them forces extra allocations.
        if (it->fence > completed) ++it;
        else it = m_completedFree.erase(it);
    }
    const auto hr = target.image.Create(m_pDevice, desc.Format, desc.Width, desc.Height, Tex2D_DefaultShaderRTarget);
    return SUCCEEDED(hr) ? S_OK : hr;
}

void CDX11VideoProcessor::RecycleCompletedFrame(CompletedFrame&& frame)
{
    frame.fence = (std::max)(frame.fence, frame.backBufferReadFence);
    if (m_completedFree.size() < 4 && TextureBytes(frame.image.desc) * 4 < m_completedByteLimit)
        m_completedFree.push_back(std::move(frame));
}

bool CDX11VideoProcessor::QueueCompletedFrame(CompletedFrame&& frame)
{
    // Depth 3D must be baked while the frame is produced, not when it is
    // presented. The completed queue intentionally runs about two seconds
    // ahead, while CZipDepth::ForFrame() only accepts a recent estimate and
    // its result slots are reused. Looking the depth up again at Present time
    // therefore makes the effect appear only intermittently.
    //
    // Render the mesh into the reusable preview texture now, while the matching
    // depth estimate still exists, then copy the result back into this frame's
    // private completed texture. The queued frame is self-contained after this.
    if (m_depth3DPreview && frame.videoOnly && !frame.picture.IsRectEmpty()) {
        const auto* depth3D = PrepareDepth3DPreview(frame, frame.picture);
        if (depth3D && depth3D != &frame.image && depth3D->pTexture) {
            m_pDeviceContext->CopyResource(frame.image.pTexture, depth3D->pTexture);
        }
    }

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

bool CDX11VideoProcessor::PrepareCompletedBatch(REFERENCE_TIME stream, bool originalPriority)
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
    const auto future = std::find_if(batch.phases.begin(), batch.phases.end(), [&](const auto& phase) {
        return phase.index != mfg::MaxGenerated && phase.presentationTime > stream;
    });
    if (!originalPriority && future != batch.phases.end() &&
        original.presentationTime - stream > CompletedSubmitLead() &&
        m_mfg.Frame(future->serial, future->index, original.timestamp).pending) return false;
    const auto started = GetPreciseTick();
    // Use the geometry and input layout recorded for this batch.
    const auto picture = batch.target.picture;
    const bool projected = batch.target.projected;
    const bool previousFormat = projected != CanProjectJvr();
    if (!batch.target.videoOnly && picture != PictureForPlayback(projected) && mfg::SourceCadenceTrace().Enabled()) {
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
        auto* dropCount = &m_completedStatsDrops.other;
        HRESULT result = S_FALSE;
        if (surface.pending) {
            drop = phase.presentationTime <= stream ? "completed_drop_ngx_late" : "completed_drop_ngx_original_due";
            dropCount = &m_completedStatsDrops.ngxLate;
        } else if (phase.presentationTime <= stream) {
            drop = "completed_drop_compose_late";
            dropCount = &m_completedStatsDrops.composeLate;
        } else if (!surface.texture) {
            drop = "completed_drop_surface";
        } else if (!m_completedFrames.empty() && phase.presentationTime <= m_completedFrames.back().due) {
            drop = "completed_drop_overlap";
        } else {
            result = AcquireCompletedTarget(batch.target.image.desc, target);
            if (result == S_FALSE) {
                drop = "completed_drop_budget";
                dropCount = &m_completedStatsDrops.budget;
            }
            else if (FAILED(result)) drop = "completed_drop_texture";
        }
        if (drop) {
            ++*dropCount;
            ++m_mfgLostGenerated;
            ++m_mfgSkipped;
            if (mfg::SourceCadenceTrace().Enabled()) {
                mfg::SourceCadenceTrace().Record(drop, GetPreciseTick(), phase.timestamp, stream,
                    phase.serial, phase.index, 0, 0, 0, false, m_mfgLostGenerated, result);
            }
            continue;
        }
        target.picture = picture;
        target.videoOnly = batch.target.videoOnly;
        target.projected = projected;
        target.pts = phase.timestamp;
        target.due = phase.presentationTime;
        target.serial = phase.serial;
        target.phase = phase.index;
        target.mfg = true;
        sampled = true;
        if (FAILED(RenderMfgVideo(surface, phase.nativeInput, target.image.pTexture, phase.timestamp,
                                  picture, batch.videoOnlyInput, previousFormat, projected)) ||
            !QueueCompletedFrame(std::move(target))) {
            ++m_completedStatsDrops.other;
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
                                   picture, batch.videoOnlyInput, previousFormat, projected);
    if (FAILED(rendered)) {
        rendered = RenderMfgVideo(surface, original.nativeInput, target.image.pTexture, original.timestamp,
                                  picture, batch.videoOnlyInput, true, projected);
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
    // Complete a deferred window resize on the presentation worker's own
    // schedule, not only during PresentCompletedVideo(). A pending resize can
    // otherwise outlive the last queued frame, and the worker would sleep
    // indefinitely while geometry-dependent admission remains restricted.
    // The 140-ms interval is measured from the last window change.
    constexpr ULONGLONG resizeSettleMs = 140;
    if (m_resizePending && GetTickCount64() - m_resizeLastChangeTick >= resizeSettleMs)
        ApplyPendingWindowResize();
    // Early B-only 256x256 thumbnail capture is independent of the actual
    // display deadline. The high-priority presenter polls this callback while
    // frames are buffered, including the first frame following a seek.
    if (!m_completedFrames.empty()) PrimeSvdLutTemporalFrames();
    if (!m_completedBatches.empty()) return true;
    if (m_completedFrames.empty()) {
        // No picture is due, but an outstanding WM_SIZE still needs a
        // swap-chain commit. Keep the worker scheduled until the resize
        // settles (including retries after a failed ResizeBuffers), without
        // producing or discarding any video frame.
        if (m_resizePending) {
            const auto elapsedMs = GetTickCount64() - m_resizeLastChangeTick;
            const auto waitMs = elapsedMs < resizeSettleMs ? resizeSettleMs - elapsedMs : 1ull;
            deadline += int64_t((std::max)(1ull, waitMs) * GetPreciseTicksPerSecond() / 1000);
            return true;
        }
        if (!m_pFilter->m_completedEndOfStream) return false;
        deadline += int64_t(GetPreciseTicksPerSecond() / 1000);
        return true;
    }
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return false;
    const auto& frame = m_completedFrames.front();
    const auto lead = CompletedBackBufferLead(frame, CompletedSubmitLead());
    const auto delta = frame.due - REFERENCE_TIME(stream) - lead;
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
    if (CompletedGeometryChanging()) {
        m_completedDisplayPacing.Reset();
    }
    return REFERENCE_TIME(m_completedDisplayPacing.Start(refresh) * 10'000);
}

REFERENCE_TIME CDX11VideoProcessor::CompletedBackBufferLead(const CompletedFrame& frame, REFERENCE_TIME submitLead) const
{
    if (!m_completedPresentWaitable || !m_completedLast || !frame.mfg || !m_mfgMultiplier ||
        m_completedBackBuffer.frameFence == frame.fence || m_bVBlankBeforePresent || CompletedGeometryChanging()) {
        return submitLead;
    }
    if (m_dRefreshPeriodMs >= 1.0 && m_dRefreshPeriodMs <= 1000.0) {
        return (std::max)(submitLead, REFERENCE_TIME(m_dRefreshPeriodMs * (UNITS / 1000)));
    }
    return submitLead;
}

bool CDX11VideoProcessor::PrepareCompletedBackBuffer(CompletedFrame& frame)
{
    const auto completed = m_completedFence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        ClearCompletedFrames(false);
        m_completedFailed = true;
        return true;
    }
    if (completed < frame.fence || !TryCompletedPresentPermit()) return false;

    const auto picture = PictureForPlayback(frame.projected);
    // Remember even a failed attempt, so the normal submit path can retry
    // the draw without the presenter looping on a bad buffer before its due time.
    m_completedBackBuffer = {frame.fence, picture, false};
    CComPtr<ID3D11Texture2D> back;
    if (FAILED(m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&back)))) return true;
    const auto hr = DrawCompletedBackBuffer(frame, back, picture, false, true);
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    const auto readFence = ++m_completedFenceValue;
    if (SUCCEEDED(m_completedContext->Signal(m_completedFence, readFence))) {
        frame.backBufferReadFence = readFence;
        // A speculative pre-draw without a Temporal B LUT must not lock an
        // ungraded backbuffer into the actual present. The worker may finish
        // before the present deadline, in which case redraw once with the LUT.
        // Normal A and RAW B retain the original pre-draw behavior.
        const bool temporalB = m_pFilter->m_svdLutRaw.load() && m_pFilter->m_svdLutTemporal.load() &&
            !m_pFilter->m_svdLutOff.load();
        const bool gradeMayArrive = temporalB && (
            // After start/seek the model may still be loading: even without a
            // submitted thumbnail, do not cache an ungraded speculative draw
            // as the final image of the new segment.
            (!m_svdLutRawTest.heldView &&
                m_svdLutRawTest.model.GetState() != svdlut::ModelWorker::State::Failed) ||
            m_svdLutRawTest.inFlight ||
            std::any_of(m_svdLutRawTest.captures.begin(), m_svdLutRawTest.captures.end(),
                [](const auto& capture) { return capture.pending || bool(capture.input); }));
        // Retry the present-time drawing only when there is real pending LUT
        // work. Avoid a redundant full-resolution redraw during cold startup
        // when neither a GPU readback nor model evaluation is in progress.
        m_completedBackBuffer.drawn = SUCCEEDED(hr) && (!gradeMayArrive || m_svdLutRawTest.active);
        m_completedBackBuffer.graded = temporalB && SUCCEEDED(hr) && m_svdLutRawTest.active;
    }
    m_pDeviceContext->Flush();
    return true;
}

bool CDX11VideoProcessor::CompletedGeometryChanging() const
{
    constexpr ULONGLONG settleMs = 500;
    return m_resizePending || (m_completedGeometryTick && GetTickCount64() - m_completedGeometryTick < settleMs);
}

bool CDX11VideoProcessor::PresentCompletedFrame()
{
    if (!m_pFilter->m_RendererLock.TryLock()) return false;
    struct Unlock { CCritSec& lock; ~Unlock() { lock.Unlock(); } } unlock{m_pFilter->m_RendererLock};
    if (m_pFilter->m_filterState != State_Running || m_pFilter->m_bFlushing || !m_pDXGISwapChain1) return true;
    auto& trace = mfg::SourceCadenceTrace();
    const bool tracing = trace.Enabled();
    const auto beforeClock = tracing ? GetPreciseTick() : 0;
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream))) return false;
    const auto clockQpc = tracing ? (beforeClock + (GetPreciseTick() - beforeClock) / 2) : 0;
    if (m_completedFrames.empty()) return m_completedBatches.empty() || PrepareCompletedBatch(stream);
    const auto submitLead = CompletedSubmitLead();
    const auto remaining = m_completedFrames.front().due - REFERENCE_TIME(stream);
    if (remaining > submitLead) {
        if (remaining <= CompletedBackBufferLead(m_completedFrames.front(), submitLead)) {
            if (PrepareCompletedBackBuffer(m_completedFrames.front())) return true;
            if (m_completedBatches.empty()) return false;
        }
        return m_completedBatches.empty() || PrepareCompletedBatch(stream);
    }
    // A waitable signal can be delayed by DWM, e.g. during occlusion. Once
    // due, try nonblocking Present even without the signal so playback can recover.
    if (!TryCompletedPresentPermit() && remaining > 0) return false;
    const auto completed = m_completedFence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        ClearCompletedFrames(false);
        m_completedFailed = true;
        return true;
    }
    RefreshCompletedMemoryBudget(completed);
    if (completed < m_completedFrames.front().fence) return false;
    auto frame = std::move(m_completedFrames.front());
    m_completedFrames.pop_front();
    bool dropGenerated = false;
    if (frame.phase != mfg::MaxGenerated && !m_completedFrames.empty()) {
        const auto& next = m_completedFrames.front();
        if (next.due <= REFERENCE_TIME(stream)) {
            dropGenerated = next.phase == mfg::MaxGenerated || next.fence <= completed;
            if (!dropGenerated) {
                // A due but unfinished synthetic frame cannot replace a ready
                // one. Still leave the next refresh available for an original.
                const auto original = std::find_if(m_completedFrames.begin(), m_completedFrames.end(),
                    [](const auto& queued) { return queued.phase == mfg::MaxGenerated; });
                const auto refresh = m_dRefreshPeriodMs >= 1.0 && m_dRefreshPeriodMs <= 1000.0 ?
                    REFERENCE_TIME(m_dRefreshPeriodMs * (UNITS / 1000)) : REFERENCE_TIME(0);
                dropGenerated = refresh <= 0 || (original != m_completedFrames.end() &&
                    original->due <= REFERENCE_TIME(stream) + refresh + submitLead);
            }
        }
    }
    if (dropGenerated) {
        if (m_completedBackBuffer.frameFence == frame.fence) InvalidateCompletedBackBuffer();
        ++m_completedStatsDrops.presentLate;
        ++m_mfgLostGenerated;
        ++m_mfgSkipped;
        if (mfg::SourceCadenceTrace().Enabled()) {
            mfg::SourceCadenceTrace().Record("completed_drop_present_late", GetPreciseTick(), frame.pts, stream,
                frame.serial, frame.phase, 0, 0, 0, true, m_mfgLostGenerated, S_FALSE);
        }
        RecycleCompletedFrame(std::move(frame));
        return true;
    }
    const auto lateness = REFERENCE_TIME(stream) - frame.due;
    m_RenderStats.syncoffset = lateness;
    const auto refresh = m_dRefreshPeriodMs > 0.0 ? 1000.0 / m_dRefreshPeriodMs : 0.0;
    const bool geometryChanging = CompletedGeometryChanging();
    if (geometryChanging) {
        // Resize/compositor lateness is transient and must not consume the
        // steady-state batch-composition window after the user stops resizing.
        m_completedDisplayPacing.Reset();
    }
    const auto started = GetPreciseTick();
    uint64_t readFence = 0;
    auto hr = PresentCompletedVideo(frame, true, &readFence);
    if (hr == DXGI_ERROR_INVALID_CALL && m_pFilter->m_bIsD3DFullscreen && SUCCEEDED(InitSwapChain(false))) {
        frame.presentRetried = true;
        hr = PresentCompletedVideo(frame, true);
    }
    const auto finished = GetPreciseTick();
    m_RenderStats.presentticks = finished - started;
    const bool busy = hr == DXGI_ERROR_WAS_STILL_DRAWING;
    // Train on successful first attempts. DXGI retries can shift the observed
    // lateness and shrink the batch composition window after a transient stall.
    if (hr == S_OK && !frame.presentRetried && !geometryChanging) {
        m_completedDisplayPacing.Observe(lateness / 10'000.0, refresh);
    }
    if (tracing && (!busy || !frame.presentRetried)) {
        const auto due = int64_t(clockQpc) + int64_t((frame.due - REFERENCE_TIME(stream)) * GetPreciseTicksPerSecond() / UNITS);
        const mfg::CadenceTrace::CompletedQueue queue{
            m_completedFrames.size() + (busy ? 1u : 0u),
            m_completedBatches.size(),
            m_completedFrames.empty() ? (busy ? frame.due : -1) : m_completedFrames.back().due,
            submitLead / 10'000.0, m_completedDisplayPacing.LeadMs(), m_completedDisplayPacing.Samples()
        };
        trace.Record(busy ? "completed_present_busy" : "completed_present", finished, frame.pts, stream,
            frame.serial, frame.phase, 0, 0, due, true, m_mfgLostGenerated, hr, m_RenderStats.presentticks, frame.due, &queue);
    }
    if (busy) {
        // Present was deliberately non-blocking. Keep ownership of this frame
        // and release RendererLock so the source/producer can make progress.
        // The presentation worker retries after ~1 ms. Generated frames may
        // still be discarded later if they truly miss their display slot; an
        // original frame is never discarded by this retry path.
        frame.presentRetried = true;
        m_completedFrames.push_front(std::move(frame));
        return false;
    }
    if (hr == S_OK && m_pFilter->m_svdLutRaw.load() &&
        m_pFilter->m_svdLutTemporal.load() && !m_pFilter->m_svdLutOff.load()) {
        // b.active is the grade state of the image actually used by the
        // successful Present (including a previously prepared backbuffer).
        // These counts exclude speculative pre-draws and failed DXGI retries.
        if (m_svdLutRawTest.active) ++m_svdLutRawTest.presentedGraded;
        else ++m_svdLutRawTest.presentedUngraded;
    }
    if (hr == S_OK) RememberPresentedSvdLutB2(frame.pts, frame.fence);
    if (hr == S_OK && frame.phase == mfg::MaxGenerated) {
        m_pFilter->ReportCompletedMediaTime(frame.pts, frame.end);
        m_pFilter->m_DrawStats.Add(GetPreciseTick());
    }
    if (hr == S_OK) {
        const auto run = m_pFilter->m_featureStatusPlaybackRun.load(std::memory_order_relaxed);
        if (m_completedPresentWaitable && frame.mfg) m_completedPreDraw.Observe(run, readFence != 0);
        if (m_bShowStats && m_mfgMultiplier) {
            m_completedCadence.Observe(run, finished, frame.pts, frame.due);
        } else if (m_completedCadence.lastTick) {
            m_completedCadence = {};
        }
        if (frame.mfg) {
            if (frame.phase == mfg::MaxGenerated) ++m_mfgRealPresents;
            else ++m_mfgGeneratedPresents;
        } else if (m_mfgMultiplier.load(std::memory_order_relaxed) >= 2) {
            ++m_mfgFallbackPresents;
        }
    }
    if (FAILED(hr) && frame.phase == mfg::MaxGenerated) ++m_mfgLostReal;
    if (!readFence) {
        readFence = ++m_completedFenceValue;
        if (FAILED(m_completedContext->Signal(m_completedFence, readFence))) readFence = 0;
    }
    if (readFence && SUCCEEDED(hr)) {
        frame.fence = readFence;
        if (m_completedLast) RecycleCompletedFrame(std::move(*m_completedLast));
        m_completedLast = std::move(frame);
    }
    return true;
}

HRESULT CDX11VideoProcessor::DrawCompletedVideo(const CompletedFrame& frame, ID3D11Texture2D* target,
                                                const CRect& picture, bool geometryChanging, const CRect* targetBounds)
{
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    CRect from;
    if (!from.IntersectRect(frame.picture, CRect(0, 0, frame.image.desc.Width, frame.image.desc.Height)) || picture.IsRectEmpty())
        return E_INVALIDARG;
    const auto& source = frame.image.desc;
    if (frame.picture == picture && source.Width == desc.Width && source.Height == desc.Height && source.Format == desc.Format) {
        m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
        m_pDeviceContext->CopyResource(target, frame.image.pTexture);
        return S_OK;
    }
    const CRect to(
        picture.left + MulDiv(from.left - frame.picture.left, picture.Width(), frame.picture.Width()),
        picture.top + MulDiv(from.top - frame.picture.top, picture.Height(), frame.picture.Height()),
        picture.left + MulDiv(from.right - frame.picture.left, picture.Width(), frame.picture.Width()),
        picture.top + MulDiv(from.bottom - frame.picture.top, picture.Height(), frame.picture.Height()));
    CComPtr<ID3D11RenderTargetView> view;
    auto hr = m_pDevice->CreateRenderTargetView(target, nullptr, &view);
    if (FAILED(hr)) return hr;
    const float black[4] = {0, 0, 0, 1};
    const CRect textureBounds(0, 0, desc.Width, desc.Height);
    const CRect& bounds = targetBounds ? *targetBounds : textureBounds;
    if (bounds != textureBounds && m_svdLut.clearViewSupported)
        m_pDeviceContext->ClearView(view, black, &bounds, 1);
    else
        m_pDeviceContext->ClearRenderTargetView(view, black);
    CRect visible;
    if (frame.videoOnly && !geometryChanging && visible.IntersectRect(to, bounds) && visible == to) {
        const auto scaled = ResizeShaderPass(frame.image, target, from, to, 0, false, &m_completedResizeTexture);
        if (SUCCEEDED(scaled)) return scaled;
    }
    return TextureCopyRect(frame.image, target, from, to, m_pPS_Simple, nullptr, 0, false, true);
}

HRESULT CDX11VideoProcessor::DrawCompletedBackBuffer(const CompletedFrame& frame, ID3D11Texture2D* target,
    const CRect& picture, bool geometryChanging, bool scheduled)
{
    auto& trace = mfg::SourceCadenceTrace();
    const bool tracing = scheduled && trace.Enabled();
    const auto started = tracing ? GetPreciseTick() : 0;
    // Use one pass while the picture rectangle is changing. The normal
    // scaler can need a new intermediate texture at each transient size.
    auto hr = DrawCompletedSvdLut(frame, target, picture, geometryChanging);
    if (hr != S_OK) hr = DrawCompletedVideo(frame, target, picture, geometryChanging);
    if (tracing) {
        const auto finished = GetPreciseTick();
        trace.Record("completed_blit", finished, frame.pts, -1,
            frame.serial, frame.phase, 0, 0, 0, true, m_mfgLostGenerated, hr, finished - started, frame.due);
    }
    return hr;
}

HRESULT CDX11VideoProcessor::PresentCompletedVideo(const CompletedFrame& frame, bool scheduled, uint64_t* preparedReadFence)
{
    // Completed video is independent of the swap-chain size. Keep continuous
    // window changes coalesced instead of forcing ResizeBuffers on every
    // presentation. While the final size is still settling, remap the current
    // destination rectangle into the last committed back buffer.
    ApplyPendingWindowResize();
    if (preparedReadFence) *preparedReadFence = 0;
    CComPtr<ID3D11Texture2D> back;
    auto hr = m_pDXGISwapChain1->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) return hr;
    CRect picture = PictureForPlayback(frame.projected);
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
    const bool geometryChanging = scheduled && CompletedGeometryChanging();
    // Present(DO_NOT_WAIT) did not consume the submitted image if it returned
    // DXGI_ERROR_WAS_STILL_DRAWING. A retry of the same frame and same DXGI
    // backbuffer does not need another video blit or another overlay draw.
    // Window changes and external paints invalidate this state; comparing the
    // current GetBuffer(0) identity avoids reusing a flipped/recreated buffer.
    const bool busyRetry = scheduled &&
        m_completedBackBuffer.busyRetryReady && m_completedBackBuffer.drawn &&
        m_completedBackBuffer.frameFence == frame.fence &&
        m_completedBackBuffer.picture == picture &&
        m_completedBackBuffer.busyRetryBuffer == back.p;
    const bool prepared = !busyRetry && scheduled && !geometryChanging && !m_bVBlankBeforePresent &&
        m_completedBackBuffer.drawn && m_completedBackBuffer.frameFence == frame.fence &&
        m_completedBackBuffer.picture == picture;
    const bool preparedGraded = m_completedBackBuffer.graded;
    InvalidateCompletedBackBuffer();
    if (busyRetry) {
        // The prior attempt already composed the final video + OSD. Preserve
        // the grade state recorded for that exact image in presentation stats.
        if (m_pFilter->m_svdLutRaw.load() && m_pFilter->m_svdLutTemporal.load() &&
            !m_pFilter->m_svdLutOff.load()) m_svdLutRawTest.active = preparedGraded;
        if (preparedReadFence) *preparedReadFence = frame.backBufferReadFence;
    } else if (prepared) {
        // A cached backbuffer was already graded (or left ungraded) earlier.
        // Do not let a later speculative draw overwrite the successful
        // Present's graded/ungraded counter with an unrelated b.active value.
        if (m_pFilter->m_svdLutRaw.load() && m_pFilter->m_svdLutTemporal.load() &&
            !m_pFilter->m_svdLutOff.load()) m_svdLutRawTest.active = preparedGraded;
        if (preparedReadFence) *preparedReadFence = frame.backBufferReadFence;
    } else {
        hr = DrawCompletedBackBuffer(frame, back, picture, geometryChanging, scheduled);
        if (FAILED(hr)) return hr;
    }
    if (!geometryChanging && m_bVBlankBeforePresent && m_pDXGIOutput) m_pDXGIOutput->WaitForVBlank();
    // Use the actual resized video rectangle when applying the grade.
    const CRect* outputPicture = (frame.projected || m_svdLut.strength) ? &picture : nullptr;
    const auto presentHr = FinishMfgPresent(back, frame.pts, 0, scheduled, outputPicture, true, busyRetry);
    if (presentHr == DXGI_ERROR_WAS_STILL_DRAWING && scheduled) {
        // Retain only the identity of the buffer; do not hold a COM reference
        // that could prevent the deferred DXGI ResizeBuffers from succeeding.
        // This cache is invalidated by a geometry update or an outside paint.
        m_completedBackBuffer.frameFence = frame.fence;
        m_completedBackBuffer.picture = picture;
        m_completedBackBuffer.drawn = true;
        m_completedBackBuffer.graded = m_pFilter->m_svdLutRaw.load() &&
            m_pFilter->m_svdLutTemporal.load() && !m_pFilter->m_svdLutOff.load() && m_svdLutRawTest.active;
        m_completedBackBuffer.busyRetryReady = true;
        m_completedBackBuffer.busyRetryBuffer = back.p;
    }
    return presentHr;
}

void CDX11VideoProcessor::ClearCompletedFrames(bool release)
{
    auto& trace = mfg::SourceCadenceTrace();
    if (trace.Enabled() && CompletedFramesPending()) {
        const mfg::CadenceTrace::CompletedQueue queue{
            m_completedFrames.size(), m_completedBatches.size(),
            m_completedFrames.empty() ? -1 : m_completedFrames.back().due,
            m_completedDisplayPacing.LeadMs(), 0, m_completedDisplayPacing.Samples()
        };
        trace.Record("completed_clear", GetPreciseTick(), -1, -1, 0, 0, 0, 0, 0,
            false, m_mfgLostGenerated, S_OK, 0, -1, &queue);
    }
    if (release) m_completedPresenter.Stop();
    InvalidateCompletedBackBuffer();
    // A new timeline (Seek/Flush/NewSegment) is not a resize recovery.
    // Restore the stable MFG bank admission behavior from its first frame.
    m_completedResizeRecovery.store(false, std::memory_order_relaxed);
    if (m_pFilter->m_svdLutRaw.load()) {
        if (m_pFilter->m_svdLutTemporal.load()) {
            // Seek/stop/flush invalidates asynchronous B work and its PTS.
            ResetSvdLutTemporalQueue();
        } else {
            m_svdLutRawTest.frames = {};
            m_svdLutRawTest.frameSlot = 0;
            m_svdLutRawTest.lastWindow = 0;
        }
    }
    m_completedFrames.clear();
    m_completedFree.clear();
    m_completedLast.reset();
    m_completedBatches.clear();
    m_completedTarget.reset();
    m_completedGroups.Clear();
    m_completedDisplayPacing.Reset();
    m_completedBufferStatsTick = 0;
    m_completedBufferPercent.reset();
    m_completedBufferLow = {};
    m_completedPreDraw = {};
    m_completedBufferStats.clear();
    m_completedStatsDrops = {};
    m_completedCadence = {};
    m_completedPresenter.Wake();
    if (release) {
        m_completedResizeTexture.Release();
        m_completedContext.Release();
        m_completedFence.Release();
        m_completedAdapter.Release();
        m_completedFenceValue = m_completedByteLimit = m_completedBudgetTick = 0;
        m_completedBudgetKnown = false;
        m_completedMemoryInfo = {};
        m_completedFailed = false;
    }
}

CDX11VideoProcessor::CompletedBufferTime CDX11VideoProcessor::GetCompletedBufferTime(REFERENCE_TIME stream) const
{
    CompletedBufferTime time;
    time.queuedFrames = m_completedFrames.size();
    if (!m_completedFence) return time;
    time.fence = m_completedFence->GetCompletedValue();
    if (time.fence == UINT64_MAX) return time;
    bool ready = true;
    std::optional<REFERENCE_TIME> previousDue;
    for (const auto& frame : m_completedFrames) {
        time.queued = (std::max)(time.queued, frame.due - stream);
        const bool increasing = !previousDue || frame.due > *previousDue;
        if (!increasing) ++time.nonIncreasing;
        ready = ready && increasing && frame.fence <= time.fence;
        if (ready) {
            ++time.readyFrames;
            time.ready = (std::max)(time.ready, frame.due - stream);
        }
        previousDue = frame.due;
    }
    return time;
}

std::wstring CDX11VideoProcessor::CompletedBufferInfo()
{
    CRefTime stream;
    if (!m_completedFence || FAILED(m_pFilter->StreamTime(stream))) return {};

    unsigned width = 0, height = 0;
    if (!m_completedFrames.empty()) {
        width = m_completedFrames.back().image.desc.Width;
        height = m_completedFrames.back().image.desc.Height;
    }
    const auto time = GetCompletedBufferTime(stream);
    const auto pool = GetCompletedPoolUsage(time.fence);
    auto info = std::format(L"\nCompleted video buffer: {} ms, {} queued, {}x{}, {:.1f} MiB estimated pool, {:.1f} MiB pool limit, {} ms queued ahead",
        time.ready / 10000, m_completedFrames.size(), width, height, double(pool.Retained()) / MiB,
        double(m_completedByteLimit) / MiB, time.queued / 10000);
    if (time.nonIncreasing) info += std::format(L", {} deadline reversals or ties", time.nonIncreasing);
    if (m_completedBudgetKnown) {
        info += std::format(L", {:.1f} MiB process local usage, {:.1f} MiB process local budget",
            double(m_completedMemoryInfo.CurrentUsage) / MiB, double(m_completedMemoryInfo.Budget) / MiB);
    }
    return info;
}
