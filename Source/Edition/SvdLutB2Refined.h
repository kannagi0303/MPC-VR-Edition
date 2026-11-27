// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// B2.1 source-calibrated tone response. Uses the existing 512 original RGB
// thumbnail samples and the actual composed B1 LUT, with balanced evidence
// per luminance zone. Unsupported zones stay neutral (gain 1.0). The legacy
// estimator remains intact in SvdLutB2Luminance.h for compatibility.
// Same three smooth GPU zones, no new shader pass, inference or GPU readback.
#include "SvdLutB2Luminance.h"
#include "SvdLutInput.h"
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <cmath>
#include <span>

namespace svdlut::b2::refined {
// Compatibility gate; false restores the original B2.1 estimator.
inline constexpr bool Enabled = true;
inline constexpr unsigned Cells = 16;
inline constexpr unsigned SamplesPerCell = 2;
inline constexpr unsigned SampleCount = Cells * Cells * SamplesPerCell;
inline constexpr unsigned SampleValues = SampleCount * 3;
using SourceSamples = std::array<float, SampleValues>;

// Sample two staggered locations per 16x16 source tile. Averaging each tile
// before evaluating a nonlinear LUT is intentionally avoided: LUT(E[RGB])
// generally differs from E[LUT(RGB)]. Stride/pixel order are the SAME as
// ReadSignature(): mapped RGBA32F, R/G/B float code values; not BGRA.
template<class Read>
inline SourceSamples ReadSamplesWith(Read read)
{
    SourceSamples output{};
    constexpr unsigned pixelsPerCell = 256 / Cells;
    for (unsigned cy = 0; cy < Cells; ++cy) {
        for (unsigned cx = 0; cx < Cells; ++cx) {
            const unsigned base = (cy * Cells + cx) * SamplesPerCell;
            const unsigned x0 = cx * pixelsPerCell;
            const unsigned y0 = cy * pixelsPerCell;
            // Anti-diagonal pattern covers unlike content on opposite sides
            // of a tile without needing a spatial filter or extra texture.
            const unsigned px[2] = {x0 + pixelsPerCell / 4,
                                    x0 + 3 * pixelsPerCell / 4};
            const unsigned py[2] = {y0 + 3 * pixelsPerCell / 4,
                                    y0 + pixelsPerCell / 4};
            for (unsigned s = 0; s < SamplesPerCell; ++s) {
                for (unsigned c = 0; c < 3; ++c)
                    output[(base + s) * 3 + c] = read(px[s], py[s], c);
            }
        }
    }
    return output;
}

inline SourceSamples ReadSamples(const uint8_t* data, size_t rowPitch)
{
    if (!data || rowPitch < input::MinPitch) return {};
    return ReadSamplesWith([&](unsigned x, unsigned y, unsigned c) {
        return input::Code(data, rowPitch, x, y, c);
    });
}

struct Point {
    std::array<float, 3> src{}, graded{};
    float sourceY = 0, targetEv = 0, sourceCodeY = 0;
};

inline std::array<float, 3> ZoneWeights(float y)
{
    const float a = Smooth(.12f, .52f, y);
    const float b = Smooth(.52f, .90f, y);
    return {(1 - a) * (1 - b), a * (1 - b), b};
}

inline float SrgbSlope(float code)
{
    if (code <= 0.0f || code >= 1.0f) return 0.0f;
    if (code <= .04045f) return 1.0f / 12.92f;
    return (2.4f / 1.055f) * std::pow((code + .055f) / 1.055f, 1.4f);
}

// 3x3 regularized normal system; only used for three legacy zone parameters.
inline bool Solve3(double m[3][4], float result[3])
{
    for (unsigned k = 0; k < 3; ++k) {
        unsigned pivot = k;
        for (unsigned r = k + 1; r < 3; ++r)
            if (std::abs(m[r][k]) > std::abs(m[pivot][k])) pivot = r;
        if (std::abs(m[pivot][k]) < 1e-9) return false;
        if (pivot != k) for (unsigned j = k; j <= 3; ++j)
            (std::swap)(m[pivot][j], m[k][j]);
        const double d = m[k][k];
        for (unsigned j = k; j <= 3; ++j) m[k][j] /= d;
        for (unsigned r = 0; r < 3; ++r) {
            if (r == k) continue;
            const double factor = m[r][k];
            for (unsigned j = k; j <= 3; ++j) m[r][j] -= factor * m[k][j];
        }
    }
    for (unsigned z = 0; z < 3; ++z) {
        result[z] = float(m[z][3]);
        if (!std::isfinite(result[z])) return false;
    }
    return true;
}

inline void BoundTone(Tone& tone)
{
    // Keep the same maximum gains and cross-zone contrast limit as B2.1.
    constexpr float limit = 1.60f / 2.2f;
    float ev[3]{};
    for (unsigned z = 0; z < 3; ++z)
        ev[z] = (std::clamp)(std::log2((std::max)(tone.gains[z], .01f)), -limit, limit);
    for (unsigned z : {0u, 2u})
        ev[z] = (std::clamp)(ev[z], ev[1] - .26f, ev[1] + .26f);
    for (unsigned z = 0; z < 3; ++z) tone.gains[z] = std::exp2(ev[z]);
}

inline float MeanError(const std::array<Point, SampleCount>& points, size_t count,
                       const Tone& tone, float* sourceResidual = nullptr)
{
    double loss = 0.0, residual = 0.0;
    if (!count) return 1e10f;
    for (size_t i = 0; i < count; ++i) {
        const auto y = LinearLuminance(ApplyTone(points[i].src, points[i].graded, tone));
        const float actualEv = std::log2((std::max)(y, 1e-5f));
        const float e = actualEv - points[i].targetEv;
        const float bounded = (std::clamp)(e, -2.0f, 2.0f);
        loss += double(bounded) * bounded;
        residual += actualEv - std::log2(points[i].sourceY);
    }
    if (sourceResidual) *sourceResidual = float(residual / count);
    return float(loss / count);
}


// Prevent a better global exposure fit from introducing new reversal on
// canonical smooth neutral/low-chroma ramps. Only reject NEW reversals:
// the SVDLUT source LUT itself may already be nonmonotonic, which is not a
// reason to change B1 or impose our own tone mapping on the source.
inline bool AddsGradientReversal(std::span<const float> lut,
                                 const Tone& legacy, const Tone& candidate)
{
    constexpr float offsets[][3]{{0,0,0}, { .04f, .01f, -.04f},
                                  {-.04f, 0, .04f}};
    for (const auto& offset : offsets) {
        float previousOld = 0, previousNew = 0;
        bool hasPrevious = false;
        for (unsigned i = 4; i <= 251; ++i) {
            const float p = i * (1.0f / 255.0f);
            const std::array<float, 3> rgb{
                Unit(p + offset[0]), Unit(p + offset[1]), Unit(p + offset[2])};
            const auto grade = ApplyB1(lut, rgb);
            const float oldY = LinearLuminance(ApplyTone(rgb, grade, legacy));
            const float newY = LinearLuminance(ApplyTone(rgb, grade, candidate));
            if (hasPrevious) {
                const float oldDelta = oldY - previousOld;
                const float newDelta = newY - previousNew;
                // New path must not invert a gradient that legacy preserved.
                if (oldDelta >= -.00001f && newDelta < -.00015f) return true;
                // A sudden, substantially larger step is also unacceptable.
                if (oldDelta >= 0.0f && newDelta > oldDelta * 1.40f + .0025f)
                    return true;
            }
            previousOld = oldY;
            previousNew = newY;
            hasPrevious = true;
        }
    }
    return false;
}

// A single, source-weighted response fit for all content; no title-card or
// face/landscape classifier. The previous estimator weighted every sample
// equally, so a 95%-dark frame dictated the gain of a 5%-white title.
// We normalize evidence *per tone zone*, so a small measured highlight does
// not inherit a completely unrelated shadow exposure correction.
struct BalancedFit {
    std::array<float, SampleCount> weight{};
    std::array<float, 3> support{};
    std::array<bool, 3> measured{};
    float weightSum = 0.0f;
};

inline BalancedFit BalanceSamples(const std::array<Point, SampleCount>& pts,
                                  size_t count)
{
    BalancedFit b{};
    for (size_t i = 0; i < count; ++i) {
        const auto zones = ZoneWeights(pts[i].sourceCodeY);
        for (unsigned k = 0; k < 3; ++k) b.support[k] += zones[k];
    }
    for (unsigned k = 0; k < 3; ++k)
        b.measured[k] = b.support[k] >= 2.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto zones = ZoneWeights(pts[i].sourceCodeY);
        for (unsigned k = 0; k < 3; ++k) {
            // Do not infer how to adjust an unsampled tone zone from another.
            if (b.measured[k]) b.weight[i] += zones[k] / b.support[k];
        }
        b.weightSum += b.weight[i];
    }
    return b;
}

inline float BalancedError(const std::array<Point, SampleCount>& pts,
                           size_t count, const Tone& tone, const BalancedFit& fit)
{
    if (fit.weightSum < .5f) return 1e10f;
    double loss = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!(fit.weight[i] > 0.0f)) continue;
        const float y = LinearLuminance(ApplyTone(pts[i].src, pts[i].graded, tone));
        const float e = (std::clamp)(std::log2((std::max)(y, 1e-5f)) -
                      pts[i].targetEv, -2.0f, 2.0f);
        loss += fit.weight[i] * double(e) * e;
    }
    return float(loss / fit.weightSum);
}

inline Result Analyze(std::span<const float> averageSignature,
                      std::span<const float> detailedRgb,
                      std::span<const float> lut)
{
    // A deliberate rollback gate retains the old B2.1 estimator. The new
    // evaluator must not silently fall back to shot-wide darkening when a
    // readback is missing: the underlying SVDLUT result is the safe default.
    if (!Enabled) return b2::Analyze(averageSignature, lut);
    if (detailedRgb.size() != SampleValues || lut.size() != Values)
        return {};

    std::array<Point, SampleCount> points{};
    std::array<float, SampleCount> shifts{};
    size_t count = 0;
    for (size_t i = 0; i < SampleCount; ++i) {
        const std::array<float, 3> src{
            Unit(detailedRgb[i * 3]), Unit(detailedRgb[i * 3 + 1]),
            Unit(detailedRgb[i * 3 + 2])};
        const float codeY = CodeLuma(src);
        const float srcY = LinearLuminance(src);
        // The upper endpoint is essential: 255/255 white titles were excluded
        // by P5's .975 cut, despite being precisely what needed calibration.
        // Exclude near-black only where quantization makes the log ratio ill posed.
        if (codeY < .025f || srcY < .001f) continue;
        const auto grade = ApplyB1(lut, src);
        const float gradeY = LinearLuminance(grade);
        const float shift = std::log2((std::max)(gradeY, .0001f) / srcY);
        if (!std::isfinite(shift)) continue;
        points[count] = {src, grade, srcY, 0, codeY};
        shifts[count++] = shift;
    }
    // No reliable data means no compensation, not an arbitrary shot-wide
    // darkening. The underlying SVDLUT remains fully active.
    if (count < 16) return {};

    const BalancedFit fit = BalanceSamples(points, count);
    if (fit.weightSum < .5f) return {};
    // Median limits noisy near-black exposure ratios, but the 90% target
    // continues to retain 10% of the model's deliberate tonal alteration.
    auto ordered = shifts;
    std::nth_element(ordered.begin(), ordered.begin() + count / 2,
                     ordered.begin() + count);
    const float median = ordered[count / 2];
    constexpr float correction = .90f;
    double avgShift = 0;
    for (size_t i = 0; i < count; ++i) {
        const float bounded = (std::clamp)(shifts[i], median - .8f, median + .8f);
        points[i].targetEv = std::log2(points[i].sourceY) +
                             (1.0f - correction) * bounded;
        avgShift += bounded;
    }

    Tone identity{};
    Tone tone{};
    // Start from identity, not the biased three-zone global estimate. Let
    // each zone move *only* when its input values were actually observed.
    constexpr float logGainLimit = 1.60f / 2.2f;
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        double normal[3][4]{};
        for (size_t i = 0; i < count; ++i) {
            if (fit.weight[i] <= 0.0f) continue;
            const auto& pt = points[i];
            const float gain = GainAt(tone, pt.sourceCodeY);
            const float head = (std::max)({pt.graded[0], pt.graded[1], pt.graded[2]});
            if (gain > 1.0f && head > 0.0f && gain >= 1.0f / head) continue;
            const auto after = ApplyTone(pt.src, pt.graded, tone);
            const float y = LinearLuminance(after);
            if (y < 1e-5f) continue;
            const float e = pt.targetEv - std::log2(y);
            const float robust = (std::min)(1.0f, .40f / (std::max)(std::abs(e), .0001f));
            float slope = 0.0f;
            constexpr float wy[3]{.2126f, .7152f, .0722f};
            for (unsigned c = 0; c < 3; ++c)
                slope += wy[c] * after[c] * SrgbSlope(after[c]);
            slope /= y;
            if (!(slope > .05f) || !std::isfinite(slope)) continue;
            const auto zone = ZoneWeights(pt.sourceCodeY);
            float j[3]{};
            for (unsigned k = 0; k < 3; ++k)
                if (fit.measured[k]) j[k] = slope * zone[k] * tone.gains[k] / gain;
            const double w = double(fit.weight[i] * robust * 100.0f);
            for (unsigned k = 0; k < 3; ++k) {
                normal[k][3] += w * j[k] * e;
                for (unsigned l = 0; l < 3; ++l)
                    normal[k][l] += w * j[k] * j[l];
            }
        }
        // A mild zero-centred prior makes unsupported/unconstrained zones
        // neutral instead of borrowing the shadow correction across the frame.
        for (unsigned k = 0; k < 3; ++k) normal[k][k] += 0.10;
        float step[3]{};
        if (!Solve3(normal, step)) return {};
        for (unsigned k = 0; k < 3; ++k) {
            if (!fit.measured[k]) {tone.gains[k] = 1.0f; continue;}
            const float updated = std::log2(tone.gains[k]) +
                                   (std::clamp)(step[k], -.20f, .20f);
            tone.gains[k] = std::exp2((std::clamp)(updated,
                                     -logGainLimit, logGainLimit));
        }
    }
    // Evaluate the actual composed LUT, not each constituent 2D plane.
    // Back off the requested correction if it introduces new tonal reversals.
    // If none is safe, preserve the unmodified model output.
    Tone safe = identity;
    const float initialError = BalancedError(points, count, identity, fit);
    float bestError = initialError;
    for (float amount = 1.0f; amount >= 1.0f/16; amount *= .5f) {
        Tone trial{};
        for (unsigned k = 0; k < 3; ++k)
            trial.gains[k] = std::exp2(amount * std::log2(tone.gains[k]));
        const float error = BalancedError(points, count, trial, fit);
        if (error + 1e-7f < bestError &&
            !AddsGradientReversal(lut, identity, trial)) {
            safe = trial;
            bestError = error;
            break; // Prefer the highest safe calibration; avoid extra full ramp scans.
        }
    }

    Result result{};
    result.tone = safe;
    result.samples = unsigned(count);
    result.sourceShiftEv = float(avgShift / count);
    double residual = 0;
    for (size_t i = 0; i < count; ++i) {
        const float y = LinearLuminance(ApplyTone(points[i].src, points[i].graded, safe));
        residual += std::log2((std::max)(y, 1e-5f) / points[i].sourceY);
    }
    result.residualEv = float(residual / count);
    return result;
}
} // namespace svdlut::b2::refined
