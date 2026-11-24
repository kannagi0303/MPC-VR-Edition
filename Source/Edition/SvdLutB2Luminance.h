// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// B2.1 experiment: original-frame-referenced zonal tone conservation.
// Operates on B1's existing 8x8 ORIGINAL-frame signature and the exact B1 LUT
// selected for presentation. Does not change B1 inference or temporal blending.
// Fixed source-code luma zones avoid scene-specific histogram remapping.
// The O(64) calibration does no iterative binary search, GPU readback or inference.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace svdlut::b2 {
inline constexpr size_t SignatureTiles = 64;
inline constexpr size_t SignatureValues = SignatureTiles * 3;
inline constexpr size_t Width = 33;
inline constexpr size_t Layers = 12;
inline constexpr size_t Values = Layers * Width * Width * 4;

struct Tone {
    // Shader constants x/y/z: source-shadow, midtone, highlight code gains.
    std::array<float, 3> gains{1.0f, 1.0f, 1.0f};
};
struct Result {
    Tone tone;
    float sourceShiftEv = 0.0f;
    float residualEv = 0.0f;
    std::array<float, 3> zoneShiftEv{};
    unsigned samples = 0;
};

inline float Unit(float v) { return (std::clamp)(v, 0.0f, 1.0f); }
inline float Smooth(float a, float b, float x)
{
    const float t = Unit((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
inline float CodeLuma(const std::array<float, 3>& rgb)
{
    return .2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2];
}
inline float SrgbToLinear(float value)
{
    value = Unit(value);
    return value <= .04045f ? value / 12.92f :
        std::pow((value + .055f) / 1.055f, 2.4f);
}
inline float LinearLuminance(const std::array<float, 3>& rgb)
{
    return .2126f * SrgbToLinear(rgb[0]) +
           .7152f * SrgbToLinear(rgb[1]) +
           .0722f * SrgbToLinear(rgb[2]);
}
// Exactly matches the fixed zones and blends used in the optional GPU shader.
inline float GainAt(const Tone& tone, float originalCodeLuma)
{
    const float lo = Smooth(.12f, .52f, originalCodeLuma);
    const float hi = Smooth(.52f, .90f, originalCodeLuma);
    const float mid = tone.gains[0] + lo * (tone.gains[1] - tone.gains[0]);
    return mid + hi * (tone.gains[2] - mid);
}
inline std::array<float, 3> ApplyTone(const std::array<float, 3>& original,
    const std::array<float, 3>& graded, const Tone& tone)
{
    float gain = GainAt(tone, CodeLuma(original));
    if (gain > 1.0f) {
        const float head = (std::max)({graded[0], graded[1], graded[2]});
        if (head > 0.0f) gain = (std::min)(gain, 1.0f / head);
    }
    return {Unit(graded[0] * gain), Unit(graded[1] * gain), Unit(graded[2] * gain)};
}

// Packed plane RGB interpolation; default interior, optional edge variant.
inline std::array<float, 3> ApplyB1(std::span<const float> lut,
    const std::array<float, 3>& rgb, unsigned variant = 0)
{
    std::array<float, 3> output{};
    if (lut.size() != Values || variant >= 4) return output;
    const size_t base = variant * 3;
    const auto plane = [&](size_t layer, float u, float v, size_t channel) {
        const float x = Unit(u) * float(Width - 1);
        const float y = Unit(v) * float(Width - 1);
        const auto x0 = static_cast<size_t>(x), y0 = static_cast<size_t>(y);
        const auto x1 = (std::min)(x0 + 1, Width - 1);
        const auto y1 = (std::min)(y0 + 1, Width - 1);
        const float tx = x - float(x0), ty = y - float(y0);
        const auto value = [&](size_t ix, size_t iy) {
            return lut[((layer * Width * Width + iy * Width + ix) * 4) + channel];
        };
        const float top = value(x0, y0) + tx * (value(x1, y0) - value(x0, y0));
        const float bottom = value(x0, y1) + tx * (value(x1, y1) - value(x0, y1));
        return top + ty * (bottom - top);
    };
    for (size_t c = 0; c < 3; ++c)
        output[c] = Unit(plane(base, rgb[0], rgb[1], c) +
                         plane(base + 1, rgb[0], rgb[2], c) +
                         plane(base + 2, rgb[1], rgb[2], c));
    return output;
}

inline Result Analyze(std::span<const float> signatureRgb, std::span<const float> b1Lut)
{
    Result result{};
    if (signatureRgb.size() != SignatureValues || b1Lut.size() != Values) return result;
    struct Sample {
        std::array<float, 3> original{}, graded{};
        float codeY = 0.0f, sourceY = 0.0f, shift = 0.0f;
    };
    std::array<Sample, SignatureTiles> samples{};
    std::array<float, SignatureTiles> shifts{};
    for (size_t i = 0; i < SignatureTiles; ++i) {
        // ReadSignature() samples DXGI_FORMAT_R8G8B8A8_UNORM bytes in RGB
        // order. Do not swap R/B here: doing so assigns the source exposure
        // and B1 LUT response to the wrong color channels.
        const std::array<float, 3> rgb{
            Unit(signatureRgb[i * 3]),
            Unit(signatureRgb[i * 3 + 1]),
            Unit(signatureRgb[i * 3 + 2])
        };
        const float codeY = CodeLuma(rgb);
        const float originalY = LinearLuminance(rgb);
        // Exclude unreliable crushed-black/near-white calibration patches;
        // do not automatically lift an intentionally dark *scene*.
        if (codeY < .035f || codeY > .975f || originalY < .0005f) continue;
        const auto graded = ApplyB1(b1Lut, rgb);
        const float gradedY = LinearLuminance(graded);
        const float shift = std::log2((std::max)(gradedY, .0001f) / originalY);
        if (!std::isfinite(shift)) continue;
        samples[result.samples] = {rgb, graded, codeY, originalY, shift};
        shifts[result.samples++] = shift;
    }
    if (result.samples < 8) return result;
    // One robust location estimate; do not perform an iterative gain search.
    std::nth_element(shifts.begin(), shifts.begin() + result.samples / 2,
        shifts.begin() + result.samples);
    const float median = shifts[result.samples / 2];
    std::array<double, 3> sums{}, weights{};
    double global = 0;
    for (size_t i = 0; i < result.samples; ++i) {
        const auto& s = samples[i];
        const float bounded = (std::clamp)(s.shift, median - .8f, median + .8f);
        global += bounded;
        const float a = Smooth(.12f, .52f, s.codeY);
        const float b = Smooth(.52f, .90f, s.codeY);
        const std::array<float, 3> w{1.0f - a, a * (1.0f - b), b};
        for (size_t z = 0; z < 3; ++z) {
            sums[z] += w[z] * bounded;
            weights[z] += w[z];
        }
    }
    const float overall = float(global / result.samples);
    result.sourceShiftEv = overall;
    // Fixed brightness bands + regularization to one *common* scene anchor.
    // Empty zones naturally fall back to the global estimate, preventing wildly
    // different gains between a dark shot and an equally lit close-up.
    constexpr float confidencePrior = 6.0f;
    constexpr float retainedIntent = .90f; // Preserve 10% of B1's luminance intention.
    for (size_t z = 0; z < 3; ++z) {
        const float shift = float((sums[z] + confidencePrior * overall) /
            (weights[z] + confidencePrior));
        result.zoneShiftEv[z] = shift;
        const float compensationEv = (std::clamp)(-retainedIntent * shift, -1.60f, 1.60f);
        // Approximate the sRGB/709 coded gain; the measured residual below
        // uses the full transfer curve to expose remaining mismatch.
        result.tone.gains[z] = std::exp2(compensationEv / 2.2f);
    }
    // Prevent an extreme shadow/highlight relative gain from flattening or
    // inverting the scene's local contrast. These limits are in CODE stops.
    for (size_t z : {size_t(0), size_t(2)}) {
        const float mid = std::log2(result.tone.gains[1]);
        result.tone.gains[z] = std::exp2((std::clamp)(std::log2(result.tone.gains[z]),
            mid - .26f, mid + .26f));
    }
    if (std::abs(overall) < .025f &&
        std::abs(result.zoneShiftEv[0]) < .05f &&
        std::abs(result.zoneShiftEv[1]) < .05f &&
        std::abs(result.zoneShiftEv[2]) < .05f)
        result.tone = {};

    double residual = 0;
    for (size_t i = 0; i < result.samples; ++i) {
        const auto& s = samples[i];
        const float afterY = LinearLuminance(ApplyTone(s.original, s.graded, result.tone));
        residual += std::log2((std::max)(afterY, .0001f) / s.sourceY);
    }
    result.residualEv = float(residual / result.samples);
    return result;
}

// B2-only *shot* reference, never B1's LUT regression. Same-shot updates
// stabilize noisy thumbnail composition, while actual lighting changes adapt
// faster. A cut explicitly resets the anchor, not a slow cross-shot fade.
inline Tone ShotUpdate(const Tone& measured, const Tone& previous,
    bool sameShot, float originalPictureDistance)
{
    if (!sameShot) return measured;
    const float alpha = (std::clamp)(.16f + 2.8f * originalPictureDistance,
        .16f, .70f);
    Tone result{};
    for (size_t z = 0; z < 3; ++z) {
        const float oldEv = std::log2(previous.gains[z]);
        const float measuredEv = std::log2(measured.gains[z]);
        result.gains[z] = std::exp2(oldEv + alpha * (measuredEv - oldEv));
    }
    return result;
}

inline bool Adjusted(const Tone& tone)
{
    return std::any_of(tone.gains.begin(), tone.gains.end(),
        [](float x) { return std::abs(x - 1.0f) > .004f; });
}
} // namespace svdlut::b2
