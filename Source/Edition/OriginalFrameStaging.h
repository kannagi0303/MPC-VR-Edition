// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <memory>
#include <utility>

namespace edition::original_stage {
// Experimental, not yet wired to the renderer. The caller must own the
// delivery serialization lock, and detach the original from the decoder.
// No callbacks, GPU work, allocations, waits, or cross-thread publication.
// PTS and end are the FINAL corrected original media timestamps (100 ns).
struct Stamp {
    std::uint64_t timeline = 0; // cancellation / segment generation
    std::uint64_t route = 0;    // compatible device, input and color geometry
    std::int64_t pts = -1;
    std::int64_t end = -1;
    std::uint64_t bytes = 0;    // actual owned decoded texture allocation estimate
};

struct Admission {
    std::uint64_t timeline = 0;
    std::uint64_t route = 0;
    // Existing Completed admission, actual contiguous Ready reserve, deadline,
    // GPU fences and resize/initialization conditions are authoritative.
    bool allow = false;
    bool consecutive = false; // caller verified source cadence, not an inferred FPS
    // Budget for the TOTAL temporary raw ownership during handoff, including
    // the old packet. Must be reserved BEFORE attempting the decoder copy.
    std::uint64_t rawBudget = 0;
};

enum class Decision { Direct, HoldFirst, Advance, DrainFirst };

template<class Payload, std::uint64_t MaxPacketBytes>
class OneOriginal {
    static_assert(MaxPacketBytes > 0);
public:
    struct Packet {
        Stamp stamp;
        // Unique owner of a caller-defined detached packet (e.g. a wrapper
        // owning a COM sample). Moving only a unique_ptr avoids releasing COM
        // objects while changing slot state under the caller lock.
        std::unique_ptr<Payload> source;
    };
    using Owned = std::optional<Packet>;

    OneOriginal() = default;
    OneOriginal(const OneOriginal&) = delete;
    OneOriginal& operator=(const OneOriginal&) = delete;
    OneOriginal(OneOriginal&&) = delete;
    OneOriginal& operator=(OneOriginal&&) = delete;

    [[nodiscard]] bool Pending() const noexcept { return bool(m_pending); }
    [[nodiscard]] const Packet* Future() const noexcept {
        // Borrow only within caller's serialization scope. No reference may
        // outlive Take/Cancel/Commit. A GPU consumer needs its own lifetime proof.
        return m_pending ? &*m_pending : nullptr;
    }
    [[nodiscard]] Decision Plan(const Stamp& next, const Admission& admission) const noexcept {
        const bool valid = admission.allow && next.timeline && next.route &&
            next.timeline == admission.timeline && next.route == admission.route &&
            next.pts >= 0 && next.end > next.pts && next.bytes && next.bytes <= MaxPacketBytes &&
            next.bytes <= admission.rawBudget;
        if (!m_pending) return valid ? Decision::HoldFirst : Decision::Direct;
        const auto& old = m_pending->stamp;
        // Never silently discard old content on route/cadence/epoch mismatch.
        // Caller drains in old route or explicitly cancels its old timeline.
        if (!valid || !admission.consecutive || next.timeline != old.timeline ||
            next.route != old.route || next.pts <= old.pts ||
            old.bytes > admission.rawBudget - next.bytes)
            return Decision::DrainFirst;
        return Decision::Advance;
    }

    // Recheck admission after allocation, before transferring ANY ownership.
    // On failure both owned inputs and the slot remain untouched. The caller
    // can drain the older packet then use the original incoming sample.
    // On Advance, previous owns N, slot owns N+1; caller can analyze N+1
    // before sending N through its original Completed admission. That analysis
    // must be nonblocking and must not change the source/grade identity.
    [[nodiscard]] Decision Commit(Owned& next, const Admission& admission, Owned& previous) noexcept {
        if (!next || !next->source || previous) return m_pending ? Decision::DrainFirst : Decision::Direct;
        const auto action = Plan(next->stamp, admission);
        if (action == Decision::Direct || action == Decision::DrainFirst) return action;
        if (m_pending) {
            previous.emplace(std::move(*m_pending));
            m_pending.reset();
        }
        m_pending.emplace(std::move(*next));
        next.reset();
        return action;
    }

    // EOS / ineligible source / route changes: transfer, do NOT release.
    // The caller must successfully deliver this before the later original or
    // EOS, outside the staging lock, or propagate the actual delivery error.
    [[nodiscard]] Owned Take() noexcept {
        Owned result;
        if (m_pending) {
            result.emplace(std::move(*m_pending));
            m_pending.reset();
        }
        return result;
    }

    // Only the owner of an explicitly cancelled timeline may retire its packet.
    // Return ownership so destruction can occur OUTSIDE renderer/queue locks.
    // A stale cancellation cannot remove a new timeline's pending original.
    [[nodiscard]] Owned Cancel(std::uint64_t cancelledTimeline) noexcept {
        if (!cancelledTimeline || !m_pending || m_pending->stamp.timeline != cancelledTimeline) return {};
        return Take();
    }
private:
    Owned m_pending;
};
} // namespace edition::original_stage
