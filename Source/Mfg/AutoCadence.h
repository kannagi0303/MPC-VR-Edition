// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mfg {
// Persisted settings value; deliberately outside the manual x2..x6 range.
inline constexpr unsigned AutoMultiplier = 7;

// One choice per pair of decoded source PTS values. Preserve fractional display
// slots by using the same epoch for every interval; never round each interval
// independently (24 -> 60 Hz must alternate x2/x3 rather than always x3).
// The first sample starts at x2 to initialise NGX/optical-flow history.
// Return x1 if no additional presentation slot exists in the interval.
// A slot is a budget, NOT a promise that the generated frame is scan-out exact.
inline unsigned AutoCadenceMultiplier(int64_t previousPts, int64_t currentPts,
                                     int64_t epochPts, double displayHz)
{
    constexpr int64_t TicksPerSecond = 10000000;
    if (previousPts < 0 || currentPts <= previousPts ||
        currentPts - previousPts > TicksPerSecond / 4 ||
        epochPts < 0 || previousPts < epochPts ||
        !std::isfinite(displayHz) || displayHz < 24.0 || displayHz > 1000.0)
        return 2; // safe warm-up / discontinuity

    const double scale = displayHz / double(TicksPerSecond);
    const double a = double(previousPts - epochPts) * scale;
    const double b = double(currentPts - epochPts) * scale;
    if (!std::isfinite(a) || !std::isfinite(b)) return 2;
    const int64_t before = static_cast<int64_t>(std::floor(a + 0.5));
    const int64_t after = static_cast<int64_t>(std::floor(b + 0.5));
    const int64_t slots = after - before;
    return static_cast<unsigned>(std::clamp<int64_t>(slots, 1, 6));
}
} // namespace mfg
