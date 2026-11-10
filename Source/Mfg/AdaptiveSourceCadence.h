// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mfg {
// An opt-in source-*interval* classifier for live/game-like changing frame
// rates. This is deliberately NOT a clock and does not change the decoded
// picture's PTS or the DirectShow/audio clock. It does not infer picture
// freshness from timestamps; a duplicate encoded frame is still unknown.
//
// The ordinary LiveFlowGroups heuristic is preserved when Stage 3.2 is OFF.
// For changing-rate experiments, a short series of consistent longer spans
// establishes a new source cadence instead of continually treating it as a
// one-off stall. Isolated gaps are still suppressed for NGX interpolation.
class AdaptiveSourceCadence {
    double usual_ = 0.0;
    double candidate_ = 0.0;
    unsigned consecutive_ = 0;
public:
    enum class Kind { Normal, IsolatedLongGap, NewRate, Invalid };
    void Reset() { usual_ = candidate_ = 0.0; consecutive_ = 0; }
    double Usual100ns() const { return usual_; }
    unsigned CandidateCount() const { return consecutive_; }
    Kind Observe(int64_t span100ns) {
        // A non-positive or extremely long timestamp interval marks a
        // discontinuity rather than evidence of a new constant frame rate.
        if (span100ns <= 0 || span100ns > 2'500'000) {
            Reset();
            return Kind::Invalid;
        }
        const double span = static_cast<double>(span100ns);
        if (usual_ <= 0.0) {
            usual_ = span;
            return Kind::Normal;
        }
        const bool longGap = span > std::max(2.5 * usual_, usual_ + 250'000.0);
        if (!longGap) {
            // A single short/normal interval cancels a suspected sustained
            // slowdown. This prevents sporadic network gaps setting the rate.
            candidate_ = 0.0;
            consecutive_ = 0;
            usual_ += .12 * (span - usual_);
            return Kind::Normal;
        }
        // A genuine slower source cadence should repeat with comparable
        // intervals; one stall (however long) is not enough evidence.
        if (consecutive_ != 0 && span >= candidate_ * .8 && span <= candidate_ * 1.2) {
            ++consecutive_;
            candidate_ += .20 * (span - candidate_);
        } else {
            consecutive_ = 1;
            candidate_ = span;
        }
        if (consecutive_ >= 3) {
            usual_ = candidate_;
            candidate_ = 0.0;
            consecutive_ = 0;
            return Kind::NewRate;
        }
        // Same gradual update as ordinary LiveFlowGroups on an odd span;
        // it is bounded and does not slide PTS or alter audio synchronization.
        usual_ += .04 * (span - usual_);
        return Kind::IsolatedLongGap;
    }
};
} // namespace mfg
