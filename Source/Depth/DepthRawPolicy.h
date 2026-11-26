// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

namespace depthraw {
// A completed map belongs to an original PTS. Raw mode never reprojects or
// interpolates depth in time. Only allow a nearby past estimate, not a future
// estimate or an arbitrarily stale map. The source cache additionally gates
// scene, generation and its existing 200 ms maximum age.
inline bool Usable(std::int64_t estimate, std::int64_t pts, float frameTimeMs) noexcept
{
    if (estimate < 0 || pts < estimate) return false;
    if (estimate == pts) return true;
    if (!std::isfinite(frameTimeMs) || frameTimeMs <= 0.f) return false;
    // One 100 ns tick of tolerance for the float duration passed by the caller.
    return double(pts - estimate) <= std::ceil(double(frameTimeMs) * 10000.0) + 1.0;
}
}
