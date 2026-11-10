// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mfg {
// Delay *work submission*, never video presentation. Unlike a presentation
// offset this does not modify source PTS, DirectShow clock, or A/V sync.
// The small bounded adjustment aims to have interpolation ready a few ms
// before its scheduled slot; the v5 original-frame policy is unchanged.
class AdaptiveLookahead {
    static constexpr std::size_t Window = 96;
    static constexpr std::size_t Warmup = 6;
    static constexpr double ReserveMs = 6.0;
    std::array<double, Window> required_{};
    std::size_t count_ = 0, next_ = 0;
    double extraMs_ = 0.0, percentileMs_ = 0.0;
public:
    void Reset() {
        count_ = next_ = 0;
        extraMs_ = percentileMs_ = 0.0;
    }
    std::size_t Samples() const {return count_;}
    double ExtraMs() const {return extraMs_;}
    double PercentileMs() const {return percentileMs_;}

    // readyLeadMs = earliest generated frame's due time minus the current
    // stream time at the END of the batch's Generate call. It can be negative.
    // Since observed lead already includes the current extra lookahead,
    // add extraMs_ back when estimating the required extra. Otherwise the
    // controller would repeatedly raise and then lower its own target.
    double Observe(double readyLeadMs, double maxExtraMs) {
        if (!std::isfinite(readyLeadMs) || !std::isfinite(maxExtraMs) || maxExtraMs <= 0)
            return extraMs_;
        maxExtraMs = std::clamp(maxExtraMs, 0.0, 12.0);
        const double required = std::clamp(extraMs_ + ReserveMs - readyLeadMs, 0.0, maxExtraMs);
        required_[next_] = required;
        next_ = (next_ + 1) % Window;
        count_ = (std::min)(count_ + 1, Window);
        if (count_ < Warmup) return extraMs_;

        auto sorted = required_;
        std::sort(sorted.begin(), sorted.begin() + count_);
        // 97th percentile: a 3% tail matters for 30fps video when even
        // one late generated frame per second is perceptible.
        const std::size_t at = (std::min)(count_-1,
            static_cast<std::size_t>(std::ceil(0.97 * double(count_-1))));
        percentileMs_ = sorted[at];
        const double difference = percentileMs_ - extraMs_;
        if (difference > 0.25) extraMs_ += (std::min)(difference, 1.0);
        else if (difference < -0.25) extraMs_ += (std::max)(difference, -0.05);
        extraMs_ = std::clamp(extraMs_, 0.0, maxExtraMs);
        return extraMs_;
    }
};
} // namespace mfg
