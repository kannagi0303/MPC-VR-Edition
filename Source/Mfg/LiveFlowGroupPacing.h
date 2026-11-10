// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mfg {
// A source-clock-compatible adaptation of the Live Flow *group* time budget:
// learn from complete generated pairs, not independently from each output
// phase. Unlike a capture-only application, MPC-VR already has an authoritative
// audio/DirectShow clock. Spend the budget on upstream lookahead, never add an
// arbitrary delay to real source PTS.
class LiveFlowGroupPacing {
    static constexpr std::size_t Window = 96;
    std::array<double, Window> required_{};
    std::size_t used_ = 0, cursor_ = 0;
    double extraMs_ = 0.0, p90Ms_ = 0.0;
public:
    void Reset() {used_ = cursor_ = 0; extraMs_ = p90Ms_ = 0.0;}
    double ExtraMs() const {return extraMs_;}
    double P90Ms() const {return p90Ms_;}
    std::size_t Samples() const {return used_;}
    double Observe(double firstPhaseLeadMs, double maxLookaheadMs) {
        if (!std::isfinite(firstPhaseLeadMs) || !std::isfinite(maxLookaheadMs)) return extraMs_;
        const double limit = std::clamp(maxLookaheadMs, 0.0, 12.0);
        if (limit <= 0.0) return extraMs_;
        // One batch is one sample. Reserve 2.5 ms for drawing/submission and
        // renderer jitter; do not train on source frames with no interpolation.
        constexpr double DrawAndSubmitReserveMs = 2.5;
        const double need = std::clamp(extraMs_ + DrawAndSubmitReserveMs - firstPhaseLeadMs,
                                       0.0, limit);
        required_[cursor_] = need;
        cursor_ = (cursor_ + 1) % Window;
        used_ = std::min(used_ + 1, Window);
        if (used_ < 6) return extraMs_;
        auto ordered = required_;
        std::sort(ordered.begin(), ordered.begin() + used_);
        p90Ms_ = ordered[std::min(used_-1, std::size_t(std::ceil(0.90*(used_-1))))];
        if (used_ == 6) { extraMs_ = p90Ms_; return extraMs_; }
        const double gap = p90Ms_ - extraMs_;
        if (gap > .5) extraMs_ += std::min(gap - .5, 1.0);
        else if (gap < -.5) extraMs_ += std::max(gap + .5, -.05);
        extraMs_ = std::clamp(extraMs_, 0.0, limit);
        return extraMs_;
    }
};
} // namespace mfg
