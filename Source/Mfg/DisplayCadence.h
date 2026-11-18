// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace mfg {
class DisplayRateGate {
    static constexpr unsigned Window = 9;
    static constexpr double BypassRatio = 1.01;
    static constexpr double ResumeRatio = 1.02;

    double displayPeriod_ = 0.0; // In source PTS ticks, before bob doubling.
    bool bypassed_ = false;
    std::optional<int64_t> lastPts_;
    std::array<int64_t, Window> timestamps_{};
    unsigned samples_ = 0;
    unsigned oppositeWindows_ = 0;

    static double DisplayPeriod(double refreshPeriodMs, bool doubleFrames) {
        return refreshPeriodMs > 0.0 && std::isfinite(refreshPeriodMs)
            ? refreshPeriodMs * (doubleFrames ? 20'000.0 : 10'000.0) : 0.0;
    }

public:
    bool Bypassed(int64_t nominalPeriod, double refreshPeriodMs, bool doubleFrames) const {
        const double display = DisplayPeriod(refreshPeriodMs, doubleFrames);
        if (display <= 0.0) return false;
        if (display == displayPeriod_) return bypassed_;
        return nominalPeriod > 0 && double(nominalPeriod) <= display * BypassRatio;
    }

    // Start a fresh window on a seek or splice, retaining the accepted rate
    // until the new segment produces enough evidence to change it.
    void BreakContinuity() {
        lastPts_.reset();
        samples_ = oppositeWindows_ = 0;
    }

    void Observe(int64_t pts, int64_t nominalPeriod, double refreshPeriodMs, bool doubleFrames, bool discontinuity) {
        const double display = DisplayPeriod(refreshPeriodMs, doubleFrames);
        if (display != displayPeriod_) {
            bypassed_ = Bypassed(nominalPeriod, refreshPeriodMs, doubleFrames);
            displayPeriod_ = display;
            BreakContinuity();
        }
        if (display <= 0.0) return;
        if (discontinuity) BreakContinuity();
        const auto end = timestamps_.begin() + samples_;
        if (lastPts_ == pts || std::find(timestamps_.begin(), end, pts) != end) return;
        lastPts_ = pts;
        timestamps_[samples_++] = pts;
        if (samples_ < Window) return;

        // With reordered input, samples from adjacent windows can leave holes
        // at the edges. Measure the interior before trimming isolated gaps.
        const bool reordered = !std::is_sorted(timestamps_.begin(), timestamps_.end());
        if (reordered) std::sort(timestamps_.begin(), timestamps_.end());
        const unsigned first = reordered ? 1 : 0;
        const unsigned last = Window - 1 - first;
        double shortest = 0.0, longest = 0.0;
        for (unsigned i = first + 1; i <= last; ++i) {
            const auto gap = double(uint64_t(timestamps_[i]) - uint64_t(timestamps_[i - 1]));
            shortest = i == first + 1 ? gap : (std::min)(shortest, gap);
            longest = (std::max)(longest, gap);
        }
        const auto span = double(uint64_t(timestamps_[last]) - uint64_t(timestamps_[first]));
        const double period = (span - shortest - longest) / (last - first - 2);
        const bool opposite = bypassed_ ? period > display * ResumeRatio : period <= display * BypassRatio;
        oppositeWindows_ = opposite ? oppositeWindows_ + 1 : 0;
        if (oppositeWindows_ == 2) {
            bypassed_ = !bypassed_;
            oppositeWindows_ = 0;
        }
        samples_ = 0;
    }
};

// Frame-budget policy adapted from the FgGateChoose principle in the bundled
// DLSS-5-MANAGER reference: do not produce interpolation frames that cannot
// fit into the output cadence. Keep the source frames, not the excess synthetic
// frames. The small 4% headroom avoids thrashing near a refresh boundary;
// the caller additionally debounces mode changes across several frames.
// 0 means the source is already fast enough for this output, so present the
// source normally without starting the NGX frame generator.
inline unsigned DisplayBudgetMultiplier(unsigned requested, double sourceFps, double refreshHz)
{
    if (requested < 2) return 0;
    requested = std::min(requested, 6u);
    if (!std::isfinite(sourceFps) || sourceFps <= 0.0 ||
        !std::isfinite(refreshHz) || refreshHz < 24.0)
        return std::min(requested, 2u); // safe start until timings are known
    const double slots = sourceFps > 0.0 ? refreshHz * 1.04 / sourceFps : 0.0;
    if (slots < 2.0) return 0;
    return std::min(requested, std::min(6u, static_cast<unsigned>(std::floor(slots))));
}
} // namespace mfg
