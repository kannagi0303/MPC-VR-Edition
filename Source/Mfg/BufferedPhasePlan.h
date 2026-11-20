// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace mfg {
struct BufferedPhase {
    unsigned index = 0;
    int64_t mediaPts = 0;
    int64_t presentationTime = 0;
};

template<unsigned MaxPhases>
struct BufferedPhasePlan {
    std::array<BufferedPhase, MaxPhases> frames{};
    unsigned count = 0;
};

template<unsigned MaxPhases>
BufferedPhasePlan<MaxPhases> PlanBufferedPhases(int64_t mediaStart, int64_t mediaEnd,
    int64_t displayStart, int64_t displayEnd, unsigned generated, unsigned slots)
{
    BufferedPhasePlan<MaxPhases> plan;
    if (slots < 2 || slots - 1 >= generated || generated > MaxPhases ||
        mediaStart < 0 || mediaEnd <= mediaStart || displayStart < 0 || displayEnd <= displayStart) return plan;

    const auto mediaSpan = mediaEnd - mediaStart;
    const auto displaySpan = displayEnd - displayStart;
    if (mediaSpan < generated + 1 || displaySpan < slots) return plan;
    const unsigned freeSlots = slots - 1;
    const auto fraction = [](int64_t span, unsigned numerator, unsigned denominator) {
        // Divide first so the intermediate product also fits on long timelines.
        return (span / denominator) * numerator + (span % denominator) * numerator / denominator;
    };
    const auto pair = [&](unsigned slot, unsigned phase) {
        return BufferedPhase{phase - 1, mediaStart + fraction(mediaSpan, phase, generated + 1),
            displayStart + fraction(displaySpan, slot, slots)};
    };

    // Keep every display slot fixed and find the ordered subset of phases
    // with the smallest total absolute media-time error.
    constexpr auto unreachable = (std::numeric_limits<int64_t>::max)() / 2;
    const auto error = [](const BufferedPhase& frame) {
        const auto distance = frame.mediaPts > frame.presentationTime ?
            frame.mediaPts - frame.presentationTime : frame.presentationTime - frame.mediaPts;
        return (std::min)(distance, unreachable / (MaxPhases + 1));
    };
    std::array<std::array<int64_t, MaxPhases + 2>, MaxPhases + 1> cost{};
    for (unsigned slot = freeSlots; slot > 0; --slot) {
        auto& row = cost[slot - 1];
        row[generated + 1] = unreachable;
        for (unsigned phase = generated; phase > 0; --phase) {
            const auto use = error(pair(slot, phase)) + cost[slot][phase + 1];
            row[phase] = (std::min)(use, row[phase + 1]);
        }
    }
    for (unsigned phase = 1; phase <= generated && plan.count < freeSlots; ++phase) {
        const auto frame = pair(plan.count + 1, phase);
        if (error(frame) + cost[plan.count + 1][phase + 1] == cost[plan.count][phase]) {
            plan.frames[plan.count++] = frame;
        }
    }
    return plan;
}
}
