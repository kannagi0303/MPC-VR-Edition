// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mfg {
// Persisted settings value; deliberately outside the manual x2..x6 range.
inline constexpr unsigned AutoMultiplier = 7;

// Display budget for a pair of retained source PTS values. Preserve fractional
// slots by using the same epoch for every interval; never round each interval
// independently (24 -> 60 Hz must alternate x2/x3 rather than always x3).
// Zero means unknown. The caller chooses the warm-up and runtime limits.
inline unsigned AutoDisplaySlots(int64_t previousPts, int64_t currentPts,
                                 int64_t epochPts, double displayHz)
{
    constexpr int64_t TicksPerSecond = 10000000;
    if (previousPts < 0 || currentPts <= previousPts ||
        currentPts - previousPts > TicksPerSecond / 4 ||
        epochPts < 0 || previousPts < epochPts ||
        !std::isfinite(displayHz) || displayHz < 24.0 || displayHz > 1000.0)
        return 0; // unknown budget during warm-up or a discontinuity

    const double scale = displayHz / double(TicksPerSecond);
    const double a = double(previousPts - epochPts) * scale;
    const double b = double(currentPts - epochPts) * scale;
    const int64_t before = static_cast<int64_t>(std::floor(a + 0.5));
    const int64_t after = static_cast<int64_t>(std::floor(b + 0.5));
    return static_cast<unsigned>(std::max<int64_t>(1, after - before));
}

inline bool RepeatFitsDisplayBudget(int64_t previousPts, int64_t repeatPts,
                                    int64_t duration, int64_t epochPts,
                                    double displayHz, unsigned maxMultiplier)
{
    // Estimate when the next source frame can close this span. On VFR the
    // generation count will still be based on the next frame's actual PTS.
    if (previousPts < 0 || repeatPts <= previousPts || duration <= 0 ||
        repeatPts > INT64_MAX - duration) return false;
    const unsigned slots = AutoDisplaySlots(previousPts, repeatPts + duration, epochPts, displayHz);
    return slots >= 2 && slots <= maxMultiplier;
}
} // namespace mfg
