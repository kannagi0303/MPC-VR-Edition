// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
namespace edition::ve {
// [VE-08 | 2026-11-24 17:09 +08:00] Optional cold-start admission; never changes producer policy.
constexpr bool MayStart(bool flushing, bool startup, bool geometry, bool bufferedRunning,
    bool hasPresented, bool mfgActive, bool clockValid, int64_t ready, std::size_t readyFrames)
{
    if (flushing || startup || geometry) return false;
    if (!bufferedRunning) return true;
    if (!hasPresented) return false;
    // No fixed water-level requirement on bypass/non-MFG paths. For MFG, allow
    // a small contiguous reserve (80 ms or four ready frames at high refresh).
    return !mfgActive || (clockValid && (ready >= 800000 || (ready > 0 && readyFrames >= 4)));
}
// 250 ms media-time activation. No timers, waits, or extra GPU pass.
constexpr float Fade(int64_t pts, int64_t start)
{
    if (pts <= start) return 0.0f;
    const auto elapsed = uint64_t(pts) - uint64_t(start);
    if (elapsed >= 2500000) return 1.0f;
    const float t = float(elapsed) / 2500000.0f;
    return t * t * (3.0f - 2.0f * t);
}
}
