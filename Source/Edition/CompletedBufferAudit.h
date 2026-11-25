// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace edition {
// [MFG-B10 | 2026-11-24 23:56 +08:00] Diagnostic-only cross-check.
// Use the exact fence snapshot from GetCompletedBufferTime; never wait on the
// GPU, change queue ordering, or use this upper bound for admission.
struct CompletedBufferAudit {
    enum class Cutoff { None, Order, Fence, OrderAndFence, NoFence };
    Cutoff cutoff = Cutoff::None;
    std::size_t signaledFrames = 0, cutoffIndex = 0;
    int64_t signaledEnd = -1, readyEnd = -1, queuedEnd = -1;
    int64_t cutoffDue = -1, priorDue = -1;
    uint64_t cutoffFence = 0;
};

template<class Frames>
CompletedBufferAudit AuditCompletedBuffer(const Frames& frames, uint64_t fence, std::size_t readyFrames)
{
    CompletedBufferAudit audit;
    if (fence == UINT64_MAX) {
        audit.cutoff = CompletedBufferAudit::Cutoff::NoFence;
        return audit;
    }
    std::optional<int64_t> previous, maxSignaled, maxReady, maxQueued;
    std::size_t index = 0;
    for (const auto& frame : frames) {
        const bool ordered = !previous || frame.due > *previous;
        const bool signaled = frame.fence <= fence;
        if (signaled) {
            ++audit.signaledFrames;
            maxSignaled = maxSignaled ? (std::max)(*maxSignaled, int64_t(frame.due)) : int64_t(frame.due);
        }
        if (index < readyFrames)
            maxReady = maxReady ? (std::max)(*maxReady, int64_t(frame.due)) : int64_t(frame.due);
        maxQueued = maxQueued ? (std::max)(*maxQueued, int64_t(frame.due)) : int64_t(frame.due);
        if (audit.cutoff == CompletedBufferAudit::Cutoff::None && (!ordered || !signaled)) {
            audit.cutoff = !ordered ? (signaled ? CompletedBufferAudit::Cutoff::Order :
                CompletedBufferAudit::Cutoff::OrderAndFence) : CompletedBufferAudit::Cutoff::Fence;
            audit.cutoffIndex = index;
            audit.cutoffDue = frame.due;
            audit.priorDue = previous.value_or(-1);
            audit.cutoffFence = frame.fence;
        }
        previous = frame.due;
        ++index;
    }
    audit.signaledEnd = maxSignaled.value_or(-1);
    audit.readyEnd = maxReady.value_or(-1);
    audit.queuedEnd = maxQueued.value_or(-1);
    return audit;
}
}
