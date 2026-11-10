// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mfg {
// One stable anchor for the complete real/interpolated picture group. The
// DirectShow stream clock remains authoritative; only sampling jitter in the
// stream->QPC conversion is smoothed. This is NOT a playback-rate conversion
// and never changes the source timestamp or the audio clock.
class LiveFlowMediaClock {
    double anchor_ = 0.0;
    bool initialized_ = false;
    int64_t lastStream_ = -1;
    double lastFrequency_ = 0.0;
public:
    void Reset() { anchor_=0; initialized_=false; lastStream_=-1; lastFrequency_=0; }
    void Observe(int64_t qpc, int64_t stream, double frequency) {
        if (!std::isfinite(frequency) || frequency <= 0.0) { Reset(); return; }
        const double observed = double(qpc) - double(stream) * frequency / 1e7;
        if (!std::isfinite(observed)) { Reset(); return; }
        const bool discontinuous = !initialized_ || frequency != lastFrequency_ ||
            (lastStream_ >= 0 && (stream < lastStream_ || stream-lastStream_ > 2'500'000)) ||
            std::abs(observed - anchor_) > 0.10 * frequency;
        if (discontinuous) {
            anchor_ = observed;
            initialized_ = true;
        } else {
            // Keep the input time grid stable between neighbouring groups.
            // Still track slow reference-clock drift without accumulating it.
            const double move = (observed-anchor_) * .20;
            const double limit = frequency * .0005; // max 0.5 ms per group
            anchor_ += std::clamp(move, -limit, limit);
        }
        lastStream_ = stream;
        lastFrequency_ = frequency;
    }
    int64_t Deadline(int64_t timestamp, int64_t delay, int64_t now,
                     double frequency, bool& valid) const {
        valid = initialized_ && std::isfinite(frequency) && frequency>0.0;
        if (!valid) return now;
        const double when = anchor_ + (double(timestamp) + double(delay)) * frequency / 1e7;
        const double distance = when - double(now);
        valid = std::isfinite(distance) && std::abs(distance) <= frequency*2.0 &&
                when < double(std::numeric_limits<int64_t>::max()) &&
                when > double(std::numeric_limits<int64_t>::min());
        return valid ? int64_t(when) : now;
    }
    double Anchor() const { return anchor_; }
};
} // namespace mfg
