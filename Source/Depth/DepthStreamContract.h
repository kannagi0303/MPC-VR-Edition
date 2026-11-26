// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>
#include <string>
#include <stdexcept>

namespace depthstream {
// A bounded, fixed-shape past_/present_ interface. No model-specific weights.
inline constexpr unsigned MaxStates = 32;
inline constexpr std::uint64_t MaxStateBytes = 64ull * 1024 * 1024;
inline bool IsPast(std::string_view name) { return name.starts_with("past_") && name.size() > 5; }
inline std::string Present(std::string_view past) {
    if (!IsPast(past)) throw std::runtime_error("Invalid depth state input name");
    return "present_" + std::string(past.substr(5));
}
struct BankCycle {
    unsigned input = 0;
    bool initialized = false;
    bool Begin(bool reset) {
        if (!reset && initialized) return false;
        input = 0;
        initialized = true;
        return true;
    }
    unsigned Output() const { return 1 - input; }
    void Advance() { input = Output(); }
};
inline bool NeedsReset(bool valid, std::uint64_t previousGeneration, std::int64_t previousPts,
    std::uint64_t generation, std::int64_t pts, std::int64_t cut) {
    return !valid || previousGeneration != generation || pts <= previousPts ||
        (previousPts >= 0 && pts - previousPts > 2'500'000) ||
        (previousPts < cut && cut <= pts);
}
}
