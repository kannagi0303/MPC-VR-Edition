// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace mfg {

// Presentation times in 100-ns units. Media PTS remain on the pictures.
struct PresentationTiming {
    int64_t previous = -1;
    int64_t current = -1;
    unsigned slots = 0;

    int64_t Start(int64_t mediaPts) const { return previous >= 0 ? previous : mediaPts; }
    int64_t End(int64_t mediaPts) const { return current >= 0 ? current : mediaPts; }
    int64_t Phase(int64_t mediaStart, int64_t mediaEnd, unsigned phase, unsigned divisions) const {
        const auto start = Start(mediaStart);
        if (slots) {
            const auto slot = (phase * slots + divisions / 2) / divisions;
            return start + (End(mediaEnd) - start) * slot / slots;
        }
        return start + (End(mediaEnd) - start) * phase / divisions;
    }
};

class RepeatFrameTiming {
    struct Interval {
        int64_t duration = 0;
        unsigned repeats = 0;
    };
    // Eight retained intervals cover two 24-in-30 repeat cycles. The period
    // is estimated from these intervals, with no assumed content frame rate.
    static constexpr unsigned Window = 8;
    std::array<Interval, Window> history_{};
    unsigned count_ = 0;
    unsigned next_ = 0;
    int64_t lastMediaPts_ = -1;
    int64_t lastPresentationPts_ = -1;
    int64_t lastSlot_ = -1;
    int64_t lastEpoch_ = -1;
    double lastRefreshHz_ = 0;
    int64_t period_ = 0;
    unsigned slots_ = 0;
public:
    void Reset() { *this = {}; }
    void ClearEvidence() { count_ = next_ = 0; period_ = 0; }
    int64_t Period() const { return period_; }
    unsigned Slots() const { return slots_; }
    PresentationTiming Observe(int64_t previous, int64_t current, int64_t inputPeriod,
        unsigned repeats, bool eligible, int64_t epoch, double refreshHz, unsigned maxMultiplier) {
        const bool continuous = previous >= 0 && previous == lastMediaPts_ && current > previous;
        const bool sameGrid = continuous && lastSlot_ >= 0 && epoch == lastEpoch_ && refreshHz == lastRefreshHz_;
        const auto start = continuous ? lastPresentationPts_ : previous;
        auto end = current;
        auto correction = int64_t{0};
        period_ = 0;
        slots_ = 0;
        const auto span = continuous ? current - previous : 0;
        // Only reshape confirmed repeats on a stable input timestamp grid.
        // Multiple repeats in a row can be an intentional hold.
        const bool plausible = eligible && continuous && epoch == lastEpoch_ && refreshHz == lastRefreshHz_ &&
            maxMultiplier >= 2 && inputPeriod >= 80'000 &&
            inputPeriod <= 800'000 && repeats <= 1 && span <= 2'500'000 &&
            span >= inputPeriod * (repeats + 1) - inputPeriod / 12 &&
            span <= inputPeriod * (repeats + 1) + inputPeriod / 12;
        if (!plausible) {
            ClearEvidence();
        } else {
            history_[next_] = {span, repeats};
            next_ = (next_ + 1) % Window;
            count_ = std::min(count_ + 1, Window);
            if (count_ >= 4) {
                int64_t sum = 0, weighted = 0;
                unsigned repeatTotal = 0;
                for (unsigned i = 0; i < count_; ++i) {
                    const auto& interval = history_[(next_ + Window - count_ + i) % Window];
                    sum += interval.duration;
                    weighted += interval.duration * i;
                    repeatTotal += interval.repeats;
                }
                if (repeatTotal && repeatTotal < count_) {
                    // Fit evenly spaced endpoints around the centre of the
                    // recent media times, with no accumulating A/V offset.
                    const auto offset = ((count_ - 1) * sum - 2 * weighted) / (2 * count_);
                    const auto limit = std::min<int64_t>(inputPeriod / 2, 166'667);
                    correction = std::clamp(offset, -limit, limit);
                    period_ = sum / count_;
                }
            }
        }
        int64_t slot = -1;
        if (epoch >= 0 && current >= epoch && std::isfinite(refreshHz) &&
            refreshHz >= 24.0 && refreshHz <= 1000.0) {
            const double refresh = 1e7 / refreshHz;
            const auto nearestSlot = [&](int64_t pts) { return std::llround(double(pts - epoch) / refresh); };
            const auto rawSlot = nearestSlot(current);
            slot = nearestSlot(current + correction);
            if (sameGrid &&
                (slot <= lastSlot_ || slot - lastSlot_ > maxMultiplier)) {
                slot = rawSlot;
                period_ = 0;
            }
            const auto snapped = epoch + std::llround(slot * refresh);
            if (continuous && snapped <= start) {
                // Keep both originals eligible for the presenter's existing
                // protection policy when their nearest refresh is occupied.
                slot = -1;
                period_ = 0;
            } else {
                end = snapped;
                if (sameGrid)
                    slots_ = unsigned(slot - lastSlot_);
            }
        } else {
            period_ = 0;
        }
        // On a cadence transition, retain the previous presentation time
        // and rejoin raw PTS while keeping the two originals in order.
        if (continuous && end <= start) end = start + 1;
        lastMediaPts_ = current;
        lastPresentationPts_ = end;
        lastSlot_ = slot;
        lastEpoch_ = epoch;
        lastRefreshHz_ = refreshHz;
        return {start, end, slots_};
    }
};

} // namespace mfg
