// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace edition {
// [MFG-B11 | 2026-11-25 09:55 +08:00] Present / producer fairness.
// A measured slow successful Present can monopolize the renderer schedule
// even with DO_NOT_WAIT. Keep a short hysteresis in PRESENT COUNTS, not a
// timer or queue-empty reset. One producer admission credit per slow present.
class CompletedPresentFairness {
public:
    bool Observe(double elapsedMs, double refreshMs) {
        if (!(refreshMs >= 1.0 && refreshMs <= 1000.0)) {
            Reset();
            return false;
        }
        const bool slow = elapsedMs >= refreshMs * 0.5;
        handoffUntil_ = 0;
        credit_ = slow;
        if (slow) fastRemaining_ = 4;
        else if (fastRemaining_) --fastRemaining_;
        return slow;
    }
    bool Active() const { return fastRemaining_ != 0; }
    bool WithinRefresh(int64_t remaining, double refreshMs) const {
        return refreshMs >= 1.0 && refreshMs <= 1000.0 &&
            remaining > -refreshMs * 10'000.0;
    }
    bool MayAdmit(int64_t remaining, double refreshMs, bool frontReady) const {
        return Active() && credit_ && frontReady && WithinRefresh(remaining, refreshMs);
    }
    // The presenter may schedule one bounded source turn on its existing
    // timer. Only an actual input lock waiter can use it; no lock-held sleep.
    void OfferSourceTurn(int64_t deadline) { handoffUntil_ = credit_ ? deadline : 0; }
    int64_t SourceTurnUntil(int64_t now, bool waiting) const {
        return credit_ && waiting && handoffUntil_ > now ? handoffUntil_ : 0;
    }
    void ConsumeAdmission() { credit_ = false; handoffUntil_ = 0; }
    void Reset() { fastRemaining_ = 0; ConsumeAdmission(); }
private:
    unsigned fastRemaining_ = 0;
    bool credit_ = false;
    int64_t handoffUntil_ = 0;
};

class CompletedBufferPolicy {
public:
    static constexpr int64_t InitialLeadMs = 2000;
    static constexpr int64_t StartupLeadMs = 250;
    static constexpr unsigned FrameLimit = 256;

    // [MFG-B08 | 2026-11-24 17:28 +08:00] Resize bank-pressure recovery.
    // Retire ready batches normally. An unfinished synthetic phase is not a
    // reason to sacrifice cadence while the contiguous ready queue can cover
    // another producer retry. Use the existing startup reserve as an emergency
    // floor, not a new buffer target. No wait, extra GPU work, or timer.
    template<class Prepare, class ReadyTime>
    static bool TryResizeBankRecovery(Prepare&& prepare, ReadyTime&& readyTime,
                                      int64_t emergencyReserve)
    {
        if (prepare(false)) return true;
        if (readyTime() >= emergencyReserve) return false;
        return prepare(true);
    }

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
