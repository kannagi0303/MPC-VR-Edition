// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace mfg {
enum class OriginalFrameProtection : unsigned char { Prefer, Force, Off };

constexpr OriginalFrameProtection NormalizeOriginalFrameProtection(unsigned value) {
    return value <= unsigned(OriginalFrameProtection::Off)
        ? static_cast<OriginalFrameProtection>(value) : OriginalFrameProtection::Force;
}

constexpr const wchar_t* OriginalFrameProtectionName(OriginalFrameProtection mode) {
    switch (mode) {
    case OriginalFrameProtection::Prefer: return L"Prefer";
    case OriginalFrameProtection::Off: return L"Off";
    default: return L"Force";
    }
}

// Called only for frames already eligible for submission. A preferred
// original can yield to an interpolation starting at that very picture,
// provided their deadlines compete within one refresh.
inline bool CanReplaceOriginal(OriginalFrameProtection mode, int64_t sourceDeadline,
    int64_t sourcePts, int64_t candidateDeadline, int64_t candidatePts,
    int64_t interpolationStart, int64_t refreshTicks) {
    return candidatePts > sourcePts && candidateDeadline >= sourceDeadline &&
        (mode == OriginalFrameProtection::Off ||
         (mode == OriginalFrameProtection::Prefer && interpolationStart == sourcePts &&
          candidateDeadline - sourceDeadline < refreshTicks));
}
}
