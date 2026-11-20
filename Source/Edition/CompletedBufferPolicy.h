// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace edition {
class CompletedBufferPolicy {
public:
    static constexpr int64_t InitialLeadMs = 2000;
    static constexpr int64_t StartupLeadMs = 250;
    static constexpr unsigned FrameLimit = 256;

    static uint64_t MemoryReserve(uint64_t budget)
    {
        constexpr uint64_t minReserve = 512ull * 1024 * 1024;
        return (std::min)(budget / 4, (std::max)(budget / 8, minReserve));
    }

    static uint64_t PoolLimit(uint64_t budget, uint64_t usage, uint64_t retained)
    {
        const auto usable = budget - MemoryReserve(budget);
        if (usage > usable) return retained - (std::min)(retained, usage - usable);
        const auto otherUsage = usage - (std::min)(retained, usage);
        return usable - otherUsage;
    }

    void Observe(uint64_t now, int64_t capacityMs, int64_t readyMs, bool pressure)
    {
        pressure_ = pressure;
        // Count sampled playback only. A pause or stalled producer must not
        // supply the time needed to raise the target.
        if (now - lastSample_ > 1000) stableSince_ = 0;
        lastSample_ = now;
        if (pressure || capacityMs < targetMs_) {
            targetMs_ = (std::min)(targetMs_, capacityMs);
            if (pressure) targetMs_ = (std::min)(targetMs_, InitialLeadMs);
            stableSince_ = 0;
            return;
        }

        constexpr int64_t stepMs = 250;
        const bool recovering = targetMs_ < InitialLeadMs;
        const auto nextTarget = recovering ? (std::min)({capacityMs, InitialLeadMs, targetMs_ + stepMs}) :
            targetMs_ + stepMs;
        if (nextTarget <= targetMs_ || nextTarget > capacityMs ||
            (!recovering && readyMs < targetMs_ - 200)) {
            stableSince_ = 0;
            return;
        }

        if (!stableSince_) stableSince_ = now;
        if (now - stableSince_ >= (recovering ? 1000u : 8000u)) {
            targetMs_ = nextTarget;
            stableSince_ = 0;
        }
    }

    int64_t TargetMs() const { return targetMs_; }
    bool Pressure() const { return pressure_; }

private:
    uint64_t lastSample_ = 0;
    uint64_t stableSince_ = 0;
    int64_t targetMs_ = InitialLeadMs;
    bool pressure_ = false;
};
}
