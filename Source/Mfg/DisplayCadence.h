// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mfg {
inline bool SourceMeetsDisplayRate(int64_t sourcePeriod, double refreshPeriodMs, bool doubleFrames = false)
{
    if (sourcePeriod <= 0 || !std::isfinite(refreshPeriodMs) || refreshPeriodMs <= 0.0) return false;
    const double sourcePeriodMs = double(sourcePeriod) / (doubleFrames ? 20'000.0 : 10'000.0);
    // Treat fractional and integer rates such as 59.94 and 60 as equivalent.
    return sourcePeriodMs <= refreshPeriodMs * 1.002;
}

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
