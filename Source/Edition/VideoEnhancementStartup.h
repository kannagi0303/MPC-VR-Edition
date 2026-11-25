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
}
