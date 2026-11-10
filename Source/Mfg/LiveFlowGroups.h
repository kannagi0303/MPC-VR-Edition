// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Playback-native frame-group scheduler for the DLSSG path. Unlike a flat
// picture queue, a completed NGX evaluation is one coherent time interval:
// its interpolated phases followed by its OWN source frame. Keep this sequence
// intact whenever the display can still accept it. Source frames are NEVER
// discarded to reduce latency or to make room for interpolation.
//
// This is an original MPC-VR implementation guided by the observed Live Flow
// system-level behavior; it does not contain Manager's copyrighted code.
#include "FrameQueue.h"
#include "LiveFlowMediaClock.h"
#include "AdaptiveSourceCadence.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <vector>

namespace mfg {

class LiveFlowGroups {
    struct Group {
        uint64_t serial = 0;
        std::vector<QueuedFrame> pictures; // phases in PTS order, original last
        size_t next = 0;
        int64_t phaseTicks = 0;             // QPC ticks per interpolated phase
    };
    std::deque<Group> groups_;
    unsigned generatedPending_ = 0;
    uint64_t groupsCompleted_ = 0;
    uint64_t phasesMissed_ = 0;
    double typicalSpan_ = 0.0; // DirectShow 100-ns units; detect cuts/stalls
    LiveFlowMediaClock clock_;
    AdaptiveSourceCadence dynamicCadence_;

    void RemoveEmptyFront() {
        while (!groups_.empty() && groups_.front().next >= groups_.front().pictures.size()) {
            ++groupsCompleted_;
            groups_.pop_front();
        }
    }
    void DropPhase(Group& group, QueueDrops& drops) {
        auto& picture = group.pictures[group.next++];
        // Original frames are the last picture of every group and cannot be
        // removed here, including when deadlines have passed.
        if (picture.index == MaxGenerated) { --group.next; return; }
        drops.Count(picture.index);
        ++phasesMissed_;
        if (generatedPending_ > 0) --generatedPending_;
    }
public:
    void Clear() {
        groups_.clear();
        generatedPending_ = 0;
        typicalSpan_ = 0.0;
        dynamicCadence_.Reset();
        clock_.Reset();
    }
    unsigned PendingOriginals() const {
        unsigned total = 0;
        for (const auto& g : groups_)
            if (g.next < g.pictures.size()) ++total; // original is always last
        return total;
    }
    unsigned PendingGenerated() const { return generatedPending_; }
    size_t PendingGroups() const { return groups_.size(); }
    uint64_t CompletedGroups() const { return groupsCompleted_; }
    uint64_t MissedPhases() const { return phasesMissed_; }

    QueueDrops Add(const Batch& batch, int64_t qpc, int64_t stream,
                   double frequency, int64_t videoDelay, double refreshHz,
                   bool nativeInput, unsigned bankCount, bool autoCadence = false,
                   bool adaptiveSourceTiming = false) {
        QueueDrops drops;
        if (!batch.serial || !batch.original) return drops;
        bankCount = std::clamp(bankCount, 2u, 4u);
        clock_.Observe(qpc, stream, frequency);
        // An NGX output bank is recyclable, not a persistent frame store.
        // Recycle only synthetic phases. Originals hold independent snapshots.
        for (auto& g : groups_) {
            if (batch.serial <= g.serial || batch.serial - g.serial < bankCount) continue;
            while (g.next < g.pictures.size() &&
                   g.pictures[g.next].index != MaxGenerated) DropPhase(g, drops);
        }
        RemoveEmptyFront();

        Group group;
        group.serial = batch.serial;
        const int64_t span = batch.previous >= 0 && batch.current > batch.previous
                           ? batch.current - batch.previous : 0;
        bool unusual = span <= 0 || span > 2'500'000;
        if (!unusual && typicalSpan_ > 0.0)
            unusual = double(span) > std::max(2.5 * typicalSpan_, typicalSpan_ + 250'000.0);
        if (span > 0 && span <= 2'500'000)
            typicalSpan_ = typicalSpan_ > 0.0
                ? typicalSpan_ + (unusual ? .04 : .12) * (double(span) - typicalSpan_)
                : double(span);
        // Stage 3.2 is strictly opt-in; the CFR Golden branch above remains
        // unchanged. Only the admission of *generated* phases is affected.
        // Never rewrite phase PTS or the original picture's deadline.
        if (adaptiveSourceTiming) {
            const auto cadence = dynamicCadence_.Observe(span);
            unusual = cadence == AdaptiveSourceCadence::Kind::Invalid ||
                      cadence == AdaptiveSourceCadence::Kind::IsolatedLongGap;
        }

        const unsigned totalPhases = std::min(batch.generated, MaxGenerated);
        unsigned phases = unusual ? 0u : totalPhases;
        // When the display cannot use every phase, select evenly distributed
        // NGX indices, preserving the source PTS and the model's phase order.
        if (phases && !autoCadence && std::isfinite(refreshHz) && refreshHz > 0.0) {
            const auto slots = unsigned(std::max(0.0, std::floor(refreshHz * double(span) / 1e7 + .05)));
            phases = std::min(phases, slots > 0 ? slots - 1u : 0u);
        }
        auto append = [&](int64_t stamp, unsigned index) {
            bool valid = false;
            const auto deadline = clock_.Deadline(stamp, videoDelay, qpc, frequency, valid);
            if (!valid && index != MaxGenerated) { drops.Count(index); return; }
            if (index != MaxGenerated && generatedPending_ >= MaxMultiplier * bankCount) {
                drops.Count(index); return;
            }
            group.pictures.push_back({deadline, stamp, batch.serial, index, nativeInput,
                                      index == MaxGenerated ? batch.original : nullptr});
            if (index != MaxGenerated) ++generatedPending_;
        };
        if (phases && span > 0) {
            const unsigned whole = totalPhases + 1u;
            for (unsigned i = 1; i <= phases; ++i) {
                const unsigned phase = std::clamp((i * whole + (phases + 1u) / 2u) / (phases + 1u), 1u, whole - 1u);
                const auto stamp = batch.previous + int64_t((double(span) * phase) / whole);
                append(stamp, phase - 1u);
            }
            const auto spanQpc = double(span) * frequency / 1e7;
            if (std::isfinite(spanQpc) && spanQpc > 0.0)
                group.phaseTicks = int64_t(spanQpc / double(phases + 1u));
        }
        // The source picture is appended even if the clock mapping, refresh
        // estimate, or entire NGX generation failed; never drop it here.
        append(batch.current, MaxGenerated);
        groups_.push_back(std::move(group));
        return drops;
    }

    bool Next(int64_t& deadline, int64_t submitLeadTicks = 0) const {
        for (const auto& group : groups_) {
            if (group.next >= group.pictures.size()) continue;
            const int64_t lead = std::max<int64_t>(0, submitLeadTicks);
            deadline = group.pictures[group.next].deadline - lead;
            return true;
        }
        return false;
    }

    using GeneratedReady = std::function<bool(uint64_t, unsigned)>;
    // Non-consuming view for the precomposition pass. A frame is not removed
    // from the group until its D3D11 draw is successfully enqueued. This is
    // essential for source-frame integrity on an allocation/device failure.
    // Preparation can begin a whole refresh before presentation. That early
    // eligibility window must NOT be used to decide that the following real
    // picture is already due: doing so silently removes NGX phases when the
    // generation step is close to one display refresh (e.g. 29.97 x2 / 60 Hz).
    // A negative guard preserves the Stage 1/default scheduling semantics.
    std::optional<QueuedFrame> PeekReady(int64_t now, int64_t leadTicks,
                                        const GeneratedReady& ready = {},
                                        int64_t sourceGuardLeadTicks = -1) const {
        if (groups_.empty()) return std::nullopt;
        const auto& group = groups_.front();
        if (group.next >= group.pictures.size()) return std::nullopt;
        const auto& picture = group.pictures[group.next];
        const int64_t eligible = now + std::max<int64_t>(0, leadTicks);
        if (picture.deadline > eligible) return std::nullopt;
        if (picture.index != MaxGenerated) {
            const auto& original = group.pictures.back();
            const int64_t maxLate = group.phaseTicks > 0 ? group.phaseTicks * 3 / 4 : 0;
            const int64_t sourceEligible = now + (sourceGuardLeadTicks < 0
                ? std::max<int64_t>(0, leadTicks) : sourceGuardLeadTicks);
            if (original.deadline <= sourceEligible ||
                (maxLate > 0 && now > picture.deadline + maxLate) ||
                (ready && !ready(picture.serial, picture.index))) return std::nullopt;
        }
        return picture;
    }
    std::optional<QueuedFrame> TakeDue(int64_t qpc, QueueDrops& drops,
                                       int64_t submitLeadTicks = 0,
                                       const GeneratedReady& ready = {},
                                       int64_t sourceGuardLeadTicks = -1) {
        const int64_t now = qpc;
        const int64_t eligible = now + std::max<int64_t>(0, submitLeadTicks);
        for (;;) {
            RemoveEmptyFront();
            if (groups_.empty()) return std::nullopt;
            auto& group = groups_.front();
            auto& first = group.pictures[group.next];
            if (first.deadline > eligible) return std::nullopt;
            if (first.index != MaxGenerated) {
                const auto& original = group.pictures.back();
                // Late phases must never be squeezed immediately in front of
                // a due source picture, producing a rapid two-picture burst.
                const int64_t sourceEligible = now + (sourceGuardLeadTicks < 0
                    ? std::max<int64_t>(0, submitLeadTicks) : sourceGuardLeadTicks);
                const bool sourceDue = original.deadline <= sourceEligible;
                const int64_t maxLate = group.phaseTicks > 0 ? group.phaseTicks * 3 / 4 : 0;
                const bool phaseTooLate = maxLate > 0 && now > first.deadline + maxLate;
                if (sourceDue || phaseTooLate) {
                    DropPhase(group, drops);
                    continue;
                }
                // When submitting a few milliseconds ahead of its PTS, an
                // asynchronous NGX output may still be in flight. Wait until
                // the scheduled time rather than removing it prematurely.
                // Once late, omit ONLY the synthetic phase: the original
                // remains queued in the same group and is never blocked.
                if (ready && !ready(first.serial, first.index)) {
                    if (now < first.deadline) return std::nullopt;
                    DropPhase(group, drops);
                    continue;
                }
            }
            QueuedFrame selected = first;
            ++group.next;
            if (selected.index != MaxGenerated && generatedPending_ > 0) --generatedPending_;
            RemoveEmptyFront();
            return selected;
        }
    }
};
} // namespace mfg
