// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
namespace edition::svdstrength {
// A <=100% follows the existing fade/strength curve exactly. Above 100%
// extends the grade to 200% in the shader, subject to gamut headroom.
inline float AScale(int percent) {
    return float(std::clamp(percent, 0, 200)) * .01f;
}
// B uses a monotonic smoothstep: 0%=identity, 100%=half of full B,
// 200%=the exact pre-P3 B result. Can be retuned independently of A.
inline float BBlend(int percent) {
    const float t = float(std::clamp(percent, 0, 200)) * .005f;
    return t * t * (3.0f - 2.0f * t);
}
} // namespace edition::svdstrength
