// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace edition::color_work {
// [FA-02 | 2026-11-25 14:36 +08:00] Existing presenter slack after its
// submit lead; reserve 2 ms CPU/driver margin and no more than four outstanding
// completed-fence signals. These are conservative initial admission heuristics,
// not estimates of GPU engine utilization or a change to producer admission.
constexpr bool MayPrime(int64_t slack, uint64_t outstanding) {
    return slack > 20'000 && outstanding <= 4;
}
constexpr bool ToneFirst(bool tone, bool ve, bool toneTurn) {
    return tone && (!ve || toneTurn);
}
}
