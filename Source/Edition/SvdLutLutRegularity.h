// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// P6 experimental *post-generation* regularity projection for SVDLUT B.
// Motivation: smoothness/monotonicity regularization used when LEARNING 3D
// LUTs (Zeng et al., TPAMI 2022; Wang et al., ICCV 2021).  This is a bounded
// inference-time approximation, NOT the pretrained models' training loss.
//
// SVDLUT stores four spatial edge variants of three additive 2D colour-pair
// LUTs. Each plane by itself is not a 3D colour transform, so we deliberately
// DO NOT impose per-plane monotonicity. Instead we lightly penalize local
// curvature of each plane, and check composed RGB transforms on representative
// grayscale/colour ramps before accepting any change.
//
// Runs on the existing low-priority SVDLUT worker, after model inference and
// before creating the GPU LUT view. No new shader, draw, readback or inference.
// A is unchanged; B/Temporal B1 opt in when starting their worker.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace svdlut::regularity {
inline constexpr bool Enabled = false; // RF1: restore the original SVDLUT output for reference fidelity.
inline constexpr size_t Side = 33;
inline constexpr size_t PlanesPerVariant = 3;
inline constexpr size_t Variants = 4;
inline constexpr size_t PlaneCount = PlanesPerVariant * Variants;
inline constexpr size_t TexelChannels = 4;
inline constexpr size_t TotalFloats = PlaneCount * Side * Side * TexelChannels;

// Low-amplitude post-hoc regularization. Cap each plane's change to avoid
// destroying a learned grade; the total RGB shift is at most ~1.2 8-bit codes.
inline constexpr float LaplaceStep = 0.30f;
inline constexpr float MaxPlaneDelta = 0.0015f;

inline size_t Index(size_t plane, size_t x, size_t y, size_t c) noexcept
{
    return (((plane * Side * Side) + y * Side + x) * TexelChannels) + c;
}

inline float Bilinear(const float* data, size_t plane, float u, float v, size_t c) noexcept
{
    u = (std::clamp)(u, 0.0f, 1.0f) * float(Side - 1);
    v = (std::clamp)(v, 0.0f, 1.0f) * float(Side - 1);
    const size_t x0 = size_t(u), y0 = size_t(v);
    const size_t x1 = (std::min)(Side - 1, x0 + 1), y1 = (std::min)(Side - 1, y0 + 1);
    const float fx = u - float(x0), fy = v - float(y0);
    const float a = data[Index(plane, x0, y0, c)];
    const float b = data[Index(plane, x1, y0, c)];
    const float d = data[Index(plane, x0, y1, c)];
    const float e = data[Index(plane, x1, y1, c)];
    return (a + fx * (b - a)) * (1.0f - fy) + (d + fx * (e - d)) * fy;
}

inline std::array<float, 3> Apply(const float* data, size_t variant,
                                 const std::array<float, 3>& rgb) noexcept
{
    const size_t base = variant * PlanesPerVariant;
    std::array<float, 3> out{};
    for (size_t c = 0; c < 3; ++c) {
        const float v = Bilinear(data, base, rgb[0], rgb[1], c) +
                        Bilinear(data, base + 1, rgb[0], rgb[2], c) +
                        Bilinear(data, base + 2, rgb[1], rgb[2], c);
        out[c] = (std::clamp)(v, 0.0f, 1.0f);
    }
    return out;
}

inline float Luminance(const std::array<float, 3>& rgb) noexcept
{
    // The source is SDR code values. This diagnostic only compares candidate
    // with baseline using the same metric; it does not set exposure targets.
    return rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
}

struct RampStats {
    double curvature = 0.0;
    bool newReversal = false;
    bool newLargeStep = false;
};

inline RampStats ExamineRamps(const float* original, const float* proposal) noexcept
{
    RampStats stats{};
    // RGB offsets keep non-neutral transforms in the acceptance test; offsets
    // are not semantic sky, foliage or skin detection.
    constexpr std::array<std::array<float, 3>, 7> offsets{{
        {0,0,0}, { .04f, 0,-.04f}, {-.04f,0,.04f},
        { .15f,-.05f,-.10f}, {-.10f,.15f,-.05f},
        {-.08f,-.03f,.11f}, { .06f,.05f,-.11f}
    }};
    for (size_t variant = 0; variant < Variants; ++variant) {
        for (const auto& offset : offsets) {
            float oldPrev = 0.0f, newPrev = 0.0f;
            float oldStepPrev = 0.0f, newStepPrev = 0.0f;
            bool havePrev = false, haveStep = false;
            for (unsigned i = 5; i <= 250; ++i) {
                const float y = float(i) / 255.0f;
                const std::array<float, 3> rgb{
                    (std::clamp)(y + offset[0], 0.0f, 1.0f),
                    (std::clamp)(y + offset[1], 0.0f, 1.0f),
                    (std::clamp)(y + offset[2], 0.0f, 1.0f)};
                const float oldY = Luminance(Apply(original, variant, rgb));
                const float newY = Luminance(Apply(proposal, variant, rgb));
                if (havePrev) {
                    const float oldStep = oldY - oldPrev;
                    const float newStep = newY - newPrev;
                    // No new reverse steps where the original ramp was flat
                    // or monotonic. Existing non-monotonicity may improve.
                    if (oldStep >= -0.00002f && newStep < -0.00015f)
                        stats.newReversal = true;
                    if (oldStep >= 0.0f &&
                        newStep > oldStep * 1.35f + .0017f)
                        stats.newLargeStep = true;
                    if (haveStep) {
                        stats.curvature += std::abs(double(newStep - newStepPrev)) -
                                           std::abs(double(oldStep - oldStepPrev));
                    }
                    oldStepPrev = oldStep;
                    newStepPrev = newStep;
                    haveStep = true;
                }
                oldPrev = oldY;
                newPrev = newY;
                havePrev = true;
            }
        }
    }
    return stats;
}

// Parameter is deliberately a template so the algorithm can be compiled and
// stress-tested without Direct3D or the DirectML model installed.
template <typename Parameters>
inline bool Regularize(Parameters& parameters)
{
    if (!Enabled) return false;
    static_assert(sizeof(parameters.lut) == TotalFloats * sizeof(float),
                  "Unexpected SVDLUT parameter layout");
    const auto* source = &parameters.lut[0][0];
    // Reject non-finite model data, not just the portions visited by ramps.
    for (size_t i = 0; i < TotalFloats; ++i)
        if (!std::isfinite(source[i])) return false;
    auto candidate = std::make_unique<Parameters>(parameters);
    auto* target = &candidate->lut[0][0];
    bool changed = false;
    for (size_t plane = 0; plane < PlaneCount; ++plane) {
        for (size_t y = 1; y + 1 < Side; ++y) {
            for (size_t x = 1; x + 1 < Side; ++x) {
                for (size_t c = 0; c < 3; ++c) {
                    const size_t idx = Index(plane, x, y, c);
                    const float center = source[idx];
                    const float mean = 0.25f * (source[Index(plane,x-1,y,c)] +
                        source[Index(plane,x+1,y,c)] +
                        source[Index(plane,x,y-1,c)] +
                        source[Index(plane,x,y+1,c)]);
                    const float rawDelta = LaplaceStep * (mean - center);
                    const float delta = (std::clamp)(rawDelta, -MaxPlaneDelta, MaxPlaneDelta);
                    if (std::abs(delta) > 1.0e-7f) {
                        target[idx] = center + delta;
                        changed = true;
                    }
                }
            }
        }
    }
    if (!changed) return false;
    const auto stats = ExamineRamps(source, target);
    // Do not trade smooth grey/colour ramps for lower per-plane curvature.
    if (stats.newReversal || stats.newLargeStep || stats.curvature > 0.006)
        return false;
    parameters = std::move(*candidate);
    return true;
}
} // namespace svdlut::regularity
