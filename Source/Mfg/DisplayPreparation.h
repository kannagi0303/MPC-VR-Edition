// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mfg {
// One display refresh of lookahead lets the D3D11 scaler enqueue its work
// *before* the final flip. This is not an additional change to media PTS.
struct DisplayPreparation {
    static double LeadMs(double hz) {
        if (!std::isfinite(hz) || hz < 24.0 || hz > 1000.0) return 0.0;
        return std::clamp(1000.0 / hz, 1.0, 20.0);
    }
    static int64_t Wake(int64_t ptsQpc, int64_t leadTicks, bool alreadyPrepared,
                        int64_t prepareTicks) {
        return ptsQpc - std::max<int64_t>(0, alreadyPrepared ? leadTicks
                                : std::max(leadTicks, prepareTicks));
    }
    static bool InSubmitWindow(int64_t now, int64_t ptsQpc, int64_t leadTicks) {
        return now >= ptsQpc - std::max<int64_t>(0, leadTicks);
    }
};
}
