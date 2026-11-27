// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Unified robust temporal LUT regression for experimental SVDLUT Temporal B.
// No DirectX dependencies. One model result per unique frame; all decisions
// are made from cached LUTs, PTS, and thumbnails, before ONE LUT synthesis.
// Engineering synthesis of robust local regression, temporal color-transform
// coherence, and color-state change preservation (not any paper's source code).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

// [VE-02 | 2026-11-24 16:39:20 +08:00] Temporal LUT statistics (former B1).
namespace svdlut::temporal {
inline constexpr size_t NeighborsEachSide = 5;
inline constexpr int64_t MaximumDistancePts = 250 * 10000; // 250 ms in 100 ns units
inline constexpr size_t MaxWindow = NeighborsEachSide * 2 + 1;
inline constexpr size_t PictureValues = 8 * 8 * 3;

// Packed model output is 12 layers of 33x33 RGBA32F. The RAW shader samples
// three two-color planes and sums their RGB corrections, clamping to [0,1].
// Probe the TRANSFORM actually seen by that shader rather than arbitrary LUT
// coefficient differences. Four edge variants are included at lower weight;
// the main interior variant dominates. These probes are NOT image pixels.
inline constexpr size_t ProbeCount = 16;
inline constexpr size_t EdgeVariants = 4;
inline constexpr size_t FeatureCount = ProbeCount * EdgeVariants * 3;
inline constexpr size_t LutWidth = 33;
inline constexpr size_t LutLayers = 12;
inline constexpr size_t PackedValues = LutLayers * LutWidth * LutWidth * 4;
inline constexpr float FeatureWeightTotal = float(ProbeCount * 3) * (1.0f + 3.0f * .15f);
using Features = std::array<float, FeatureCount>;

struct Sample {
    const float* values = nullptr;  // packed model LUT, length given by caller
    const float* picture = nullptr; // 8x8 thumbnail RGB signature in [0,1]
    int64_t pts = 0;
    const float* color = nullptr;   // FeatureCount cached color-transform probes
};

// The positive coefficients actually traversed by the synthesis loop. Keep
// this distinct from the conservative candidate/dependency span: even a
// zero-weight candidate may affect robust selection and regression.
struct Contributions {
    struct Term { int64_t pts = 0; float coefficient = 0.0f; };
    std::array<Term, MaxWindow> terms{};
    size_t count = 0;

    double ShareAfter(int64_t pts) const noexcept {
        double total = 0.0, future = 0.0;
        for (size_t i = 0; i < count; ++i) {
            const auto& term = terms[i];
            total += term.coefficient;
            if (term.pts > pts) future += term.coefficient;
        }
        return total > 0.0 ? future / total : 0.0;
    }
};

struct Summary {
    unsigned used = 0;
    unsigned rejected = 0;
    unsigned future = 0;
    bool blended = false;
    Contributions contributions{};
};

inline float ClampUnit(float value) { return (std::clamp)(value, 0.0f, 1.0f); }

inline bool BuildColorFeatures(std::span<const float> packed, Features& result)
{
    if (packed.size() != PackedValues) return false;
    // Neutral tones, skin-relevant tones, primaries, and mixed saturations.
    // Each feature has the same semantics as SampleLevel() in the RAW shader.
    constexpr std::array<std::array<float, 3>, ProbeCount> probes{{
        {{.08f, .08f, .08f}}, {{.22f, .22f, .22f}},
        {{.40f, .40f, .40f}}, {{.60f, .60f, .60f}},
        {{.80f, .80f, .80f}}, {{.94f, .94f, .94f}},
        {{.68f, .45f, .33f}}, {{.43f, .28f, .20f}},
        {{.82f, .25f, .23f}}, {{.30f, .75f, .26f}},
        {{.25f, .36f, .85f}}, {{.82f, .73f, .28f}},
        {{.72f, .27f, .63f}}, {{.24f, .72f, .68f}},
        {{.68f, .59f, .46f}}, {{.23f, .34f, .47f}}
    }};
    const auto plane = [&](size_t layer, float u, float v, size_t channel) {
        const float fx = ClampUnit(u) * float(LutWidth - 1);
        const float fy = ClampUnit(v) * float(LutWidth - 1);
        const size_t x0 = static_cast<size_t>(fx), y0 = static_cast<size_t>(fy);
        const size_t x1 = (std::min)(x0 + 1, LutWidth - 1);
        const size_t y1 = (std::min)(y0 + 1, LutWidth - 1);
        const float tx = fx - float(x0), ty = fy - float(y0);
        const auto tap = [&](size_t x, size_t y) {
            return packed[((layer * LutWidth * LutWidth + y * LutWidth + x) * 4) + channel];
        };
        const float top = tap(x0, y0) + tx * (tap(x1, y0) - tap(x0, y0));
        const float bottom = tap(x0, y1) + tx * (tap(x1, y1) - tap(x0, y1));
        return top + ty * (bottom - top);
    };
    for (size_t variant = 0; variant < EdgeVariants; ++variant) {
        for (size_t i = 0; i < probes.size(); ++i) {
            const auto& rgb = probes[i];
            for (size_t c = 0; c < 3; ++c) {
                const float value = plane(variant * 3, rgb[0], rgb[1], c) +
                    plane(variant * 3 + 1, rgb[0], rgb[2], c) +
                    plane(variant * 3 + 2, rgb[1], rgb[2], c);
                result[(variant * ProbeCount + i) * 3 + c] = ClampUnit(value);
            }
        }
    }
    return true;
}

inline float PictureDistance(const Sample& a, const Sample& b)
{
    float total = 0.0f;
    for (size_t k = 0; k < PictureValues; ++k)
        total += std::abs(a.picture[k] - b.picture[k]);
    return total / PictureValues;
}

inline float MeanRgbShift(const Sample& a, const Sample& b)
{
    std::array<float, 3> diff{};
    for (size_t i = 0; i < PictureValues; i += 3)
        for (size_t c = 0; c < 3; ++c) diff[c] += a.picture[i + c] - b.picture[i + c];
    return (std::abs(diff[0]) + std::abs(diff[1]) + std::abs(diff[2])) / PictureValues;
}

inline bool PictureBoundary(const Sample& a, const Sample& b)
{
    // Only limits a statistical window. Does NOT reuse A's scene LUT.
    unsigned changed = 0, strong = 0;
    float meanLumaDelta = 0.0f;
    for (size_t tile = 0; tile < 64; ++tile) {
        const size_t i = tile * 3;
        const float d = (std::abs(a.picture[i] - b.picture[i]) +
            std::abs(a.picture[i + 1] - b.picture[i + 1]) +
            std::abs(a.picture[i + 2] - b.picture[i + 2])) / 3.0f;
        changed += d > 0.16f;
        strong += d > 0.27f;
        const float la = .2126f * a.picture[i] + .7152f * a.picture[i + 1] + .0722f * a.picture[i + 2];
        const float lb = .2126f * b.picture[i] + .7152f * b.picture[i + 1] + .0722f * b.picture[i + 2];
        meanLumaDelta += lb - la;
    }
    return strong >= 56 || (changed >= 48 && std::abs(meanLumaDelta / 64) > 0.11f);
}

// Temporal B uses a more sensitive *adjacent ORIGINAL-frame* cut test than
// Normal A's long-lived scene LUT. A cut is a spatial discontinuity, not just
// a global exposure change: uniform stage lights must remain in one segment.
inline bool OriginalFrameCut(const Sample& a, const Sample& b)
{
    if (!a.picture || !b.picture) return true;
    // ReadSignature() reads R8G8B8A8_UNORM thumbnails in R, G, B order.
    // Use the same RGB luma weights as PictureBoundary() above.
    float lumaA = 0.0f, lumaB = 0.0f;
    for (size_t i = 0; i < PictureValues; i += 3) {
        lumaA += .2126f * a.picture[i] + .7152f * a.picture[i + 1] + .0722f * a.picture[i + 2];
        lumaB += .2126f * b.picture[i] + .7152f * b.picture[i + 1] + .0722f * b.picture[i + 2];
    }
    lumaA /= 64.0f;
    lumaB /= 64.0f;
    float spatial = 0.0f, rgbDistance = 0.0f;
    unsigned widespread = 0, pronounced = 0;
    for (size_t i = 0; i < PictureValues; i += 3) {
        const float da = (std::abs(a.picture[i] - b.picture[i]) +
            std::abs(a.picture[i + 1] - b.picture[i + 1]) +
            std::abs(a.picture[i + 2] - b.picture[i + 2])) / 3.0f;
        const float la = .2126f * a.picture[i] + .7152f * a.picture[i + 1] + .0722f * a.picture[i + 2];
        const float lb = .2126f * b.picture[i] + .7152f * b.picture[i + 1] + .0722f * b.picture[i + 2];
        rgbDistance += da;
        spatial += std::abs((la - lumaA) - (lb - lumaB));
        widespread += da > .105f;
        pronounced += da > .20f;
    }
    const float meanDistance = rgbDistance / 64.0f;
    const float structuralDistance = spatial / 64.0f;
    return (widespread >= 36 && meanDistance > .115f && structuralDistance > .057f) ||
        (pronounced >= 22 && meanDistance > .145f && structuralDistance > .065f);
}

struct SceneProbe {
    const float* picture = nullptr;
    int64_t pts = 0;
};

// The caller supplies ready ORIGINAL-frame thumbnail signatures. Check each
// adjacent original segment rather than comparing two far-apart pictures:
// camera motion accumulates over time, while an edit cut is abrupt.
inline bool SameOriginalScene(std::span<const SceneProbe> known, SceneProbe first, SceneProbe last)
{
    if (!first.picture || !last.picture) return false;
    if (first.pts > last.pts) std::swap(first, last);
    if (last.pts - first.pts > MaximumDistancePts) return false;
    std::array<SceneProbe, 20> sequence{};
    size_t n = 0;
    sequence[n++] = first;
    for (const auto& probe : known) {
        if (!probe.picture || probe.pts <= first.pts || probe.pts >= last.pts) continue;
        if (n < sequence.size() - 1) sequence[n++] = probe;
    }
    sequence[n++] = last;
    std::sort(sequence.begin(), sequence.begin() + n, [](const auto& a, const auto& b) {
        return a.pts < b.pts;
    });
    for (size_t i = 1; i < n; ++i) {
        if (sequence[i].pts == sequence[i - 1].pts) continue;
        const Sample a{nullptr, sequence[i - 1].picture, sequence[i - 1].pts, nullptr};
        const Sample b{nullptr, sequence[i].picture, sequence[i].pts, nullptr};
        if (OriginalFrameCut(a, b)) return false;
        // If no original thumbnail has been observed for too long, do not
        // assert a continuous scene merely because the two ends look alike.
        if (b.pts - a.pts > 125 * 10000 && PictureDistance(a, b) > .040f)
            return false;
    }
    return true;
}

inline float FeatureDistance(const Sample& a, const Sample& b)
{
    float total = 0.0f;
    for (size_t v = 0; v < EdgeVariants; ++v) {
        const float importance = v == 0 ? 1.0f : .15f;
        for (size_t k = 0; k < ProbeCount * 3; ++k)
            total += importance * std::abs(a.color[v * ProbeCount * 3 + k] - b.color[v * ProbeCount * 3 + k]);
    }
    return total / FeatureWeightTotal;
}

inline float Median(std::array<float, MaxWindow> values, size_t count)
{
    if (!count) return 0.0f;
    const auto first = values.begin(), last = first + count;
    const auto mid = first + count / 2;
    std::nth_element(first, mid, last);
    const float high = *mid;
    if (count % 2) return high;
    const auto previous = mid - 1;
    std::nth_element(first, previous, mid);
    return (high + *previous) * .5f;
}

// The complete operation: robust sample selection -> ONE regularized weighted
// local-linear estimate of the target transform -> ONE full-LUT synthesis.
// No model/texture work, unbounded allocation, iterative full-LUT passes,
// smoothing stage, or additional GPU pass. Sample order MUST be by true PTS.
// Missing future samples are normal; same-PTS duplicate frames have no extra
// statistical weight. False = use the existing unmodified RAW B texture.
inline Summary Blend(std::span<const Sample> ordered, size_t center, std::span<float> output)
{
    Summary result;
    if (ordered.empty() || center >= ordered.size() || output.empty()) return result;
    const auto& target = ordered[center];
    if (!target.values || !target.picture || !target.color) return result;
    std::array<size_t, MaxWindow> indices{};
    size_t n = 0;
    indices[n++] = center;
    for (size_t cursor = center, included = 0; cursor > 0 && included < NeighborsEachSide;) {
        const size_t previous = cursor - 1;
        const auto& a = ordered[previous];
        const auto& b = ordered[cursor];
        if (!a.values || !a.picture || !a.color || a.pts > b.pts) break;
        if (a.pts == b.pts) { cursor = previous; continue; } // repeated PTS, not an extra sample
        if (b.pts - a.pts > MaximumDistancePts || target.pts - a.pts > MaximumDistancePts ||
            OriginalFrameCut(a, b)) break;
        indices[n++] = previous;
        ++included;
        cursor = previous;
    }
    for (size_t cursor = center, included = 0; cursor + 1 < ordered.size() && included < NeighborsEachSide;) {
        const size_t next = cursor + 1;
        const auto& a = ordered[cursor];
        const auto& b = ordered[next];
        if (!b.values || !b.picture || !b.color || a.pts > b.pts) break;
        if (a.pts == b.pts) { cursor = next; continue; }
        if (b.pts - a.pts > MaximumDistancePts || b.pts - target.pts > MaximumDistancePts ||
            OriginalFrameCut(a, b)) break;
        indices[n++] = next;
        ++included;
        cursor = next;
    }
    if (n < 2) { result.used = 1; return result; }

    // A two-result interval cannot establish which LUT is an outlier. If they
    // are dramatically different while the picture is stable, keep RAW B.
    if (n == 2) {
        const auto& other = ordered[indices[1]];
        if (FeatureDistance(target, other) > .045f && PictureDistance(target, other) < .025f) {
            result.used = 1;
            return result;
        }
    }

    // Medoid is a robust INITIAL anchor, not the final filtered LUT.
    size_t medoid = 0;
    float lowest = 1e30f;
    for (size_t i = 0; i < n; ++i) {
        float cost = 0.0f;
        for (size_t j = 0; j < n; ++j)
            cost += FeatureDistance(ordered[indices[i]], ordered[indices[j]]);
        if (cost < lowest) { lowest = cost; medoid = i; }
    }
    std::array<float, MaxWindow> dissimilar{}, sorted{};
    for (size_t i = 0; i < n; ++i)
        dissimilar[i] = FeatureDistance(ordered[indices[i]], ordered[indices[medoid]]);
    const float mid = Median(dissimilar, n);
    for (size_t i = 0; i < n; ++i) sorted[i] = std::abs(dissimilar[i] - mid);
    const float mad = Median(sorted, n);
    const float medoidLimit = mid + (std::max)(.012f, 2.5f * 1.4826f * mad);
    // A persistent alternating pair of plausible transforms is NOT a set of
    // isolated outliers. With an odd window, its medoid changes every frame;
    // rejecting the minority group can actually INVERT visible flicker.
    // Suppress exceptional transforms only when they are a sparse minority.
    const size_t sparseLimit = (std::max)(size_t(1), n / 4);
    size_t farCount = 0;
    for (size_t i = 0; i < n; ++i) farCount += dissimilar[i] > medoidLimit;
    const bool sparseMedoid = farCount > 0 && farCount <= sparseLimit;

    // Lightweight curvature from adjacent TRANSFORMS: a one-frame spike has
    // a large deviation from the time-interpolated neighborhood; a smooth
    // ramp does not. The corresponding picture change attenuates rejection.
    std::array<float, MaxWindow> curvature{};
    if (n >= 3) {
        std::array<size_t, MaxWindow> chronological{};
        std::copy_n(indices.begin(), n, chronological.begin());
        std::sort(chronological.begin(), chronological.begin() + n,
            [&](size_t a, size_t b) { return ordered[a].pts < ordered[b].pts; });
        for (size_t j = 1; j + 1 < n; ++j) {
            const auto& prev = ordered[chronological[j - 1]];
            const auto& here = ordered[chronological[j]];
            const auto& next = ordered[chronological[j + 1]];
            const double dt = double(next.pts) - double(prev.pts);
            if (dt <= 0) continue;
            const float t = float((double(here.pts) - double(prev.pts)) / dt);
            float error = 0.0f;
            for (size_t k = 0; k < FeatureCount; ++k) {
                const float predicted = prev.color[k] + t * (next.color[k] - prev.color[k]);
                const float importance = k / (ProbeCount * 3) == 0 ? 1.0f : .15f;
                error += importance * std::abs(here.color[k] - predicted);
            }
            error /= FeatureWeightTotal;
            const auto located = std::find(indices.begin(), indices.begin() + n, chronological[j]);
            if (located != indices.begin() + n) curvature[located - indices.begin()] = error;
        }
    }

    size_t highCurvatureCount = 0;
    for (size_t i = 0; i < n; ++i)
        highCurvatureCount += curvature[i] > .020f && dissimilar[i] > medoidLimit * .5f;
    const bool sparseCurvature = highCurvatureCount > 0 && highCurvatureCount <= sparseLimit;

    std::array<float, MaxWindow> weights{}, seconds{};
    float largestPictureShift = 0.0f;
    for (size_t i = 0; i < n; ++i)
        largestPictureShift = (std::max)(largestPictureShift, MeanRgbShift(ordered[indices[i]], target));
    // A nearly unchanged ORIGINAL picture can use more of the same-scene
    // window to suppress frame-to-frame model noise. Real illumination
    // changes shrink the effective radius immediately; the cut test above
    // has already excluded unrelated shots. This replaces the time kernel,
    // not a second smoothing/filter pass or additional LUT synthesis.
    const float stablePicture = 1.0f - ClampUnit((largestPictureShift - .008f) / .030f);
    const float timeRadius = .250f + .100f * stablePicture;
    float sum = 0.0f, first = 0.0f, second = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const auto& candidate = ordered[indices[i]];
        seconds[i] = float((double(candidate.pts) - double(target.pts)) / 1.0e7);
        const float normalizedTime = std::abs(seconds[i]) / timeRadius;
        float w = 1.0f / (1.0f + 4.0f * normalizedTime * normalizedTime);
        const float pictureDelta = PictureDistance(candidate, target);
        w /= 1.0f + 3.0f * pictureDelta;
        if (n >= 3 && sparseMedoid && dissimilar[i] > medoidLimit) {
            // Continuous confidence: avoid making a nearby legitimate color
            // change a hard cut, but suppress isolated far-away transforms.
            const float ratio = medoidLimit / dissimilar[i];
            w *= ratio * ratio;
            if (ratio < .34f) w = 0.0f;
        }
        if (n >= 3 && sparseCurvature && curvature[i] > .020f && dissimilar[i] > medoidLimit * .5f) {
            const float context = (std::max)(pictureDelta, .005f);
            const float excess = curvature[i] / (.020f + 2.0f * context);
            if (excess > 1.0f) w /= excess * excess;
        }
        if (w < 1e-5f) { w = 0.0f; ++result.rejected; }
        weights[i] = w;
        sum += w;
        first += w * seconds[i];
        second += w * seconds[i] * seconds[i];
    }
    if (!(sum > 0)) { result.used = 1; return result; }

    // One robust-regression refinement from small color features; the full
    // packed LUT has NOT been touched yet. Regularization limits unstable
    // extrapolation when only past results are available. It is weak when
    // source content really changes and stronger for nearly static content.
    if (n >= 4 && (second * sum - first * first) > 1e-7f) {
        std::array<float, FeatureCount> mean{}, slope{};
        for (size_t i = 0; i < n; ++i) {
            if (!weights[i]) continue;
            const auto* f = ordered[indices[i]].color;
            for (size_t k = 0; k < FeatureCount; ++k) {
                mean[k] += weights[i] * f[k];
                slope[k] += weights[i] * seconds[i] * f[k];
            }
        }
        const float centerTime = first / sum;
        const float variance = second - first * first / sum;
        for (size_t k = 0; k < FeatureCount; ++k) {
            mean[k] /= sum;
            slope[k] = (slope[k] - first * mean[k]) / variance;
        }
        std::array<float, MaxWindow> residual{};
        for (size_t i = 0; i < n; ++i) {
            const auto* f = ordered[indices[i]].color;
            for (size_t k = 0; k < FeatureCount; ++k) {
                const float importance = k / (ProbeCount * 3) == 0 ? 1.0f : .15f;
                residual[i] += importance * std::abs(f[k] - mean[k] - slope[k] * (seconds[i] - centerTime));
            }
            residual[i] /= FeatureWeightTotal;
        }
        const float middle = Median(residual, n);
        for (size_t i = 0; i < n; ++i) sorted[i] = std::abs(residual[i] - middle);
        const float spread = Median(sorted, n);
        const float limit = middle + (std::max)(.008f, 2.5f * 1.4826f * spread);
        size_t residualOutliers = 0;
        for (size_t i = 0; i < n; ++i)
            residualOutliers += weights[i] && residual[i] > limit;
        // Reweight only sparse anomalies. Two sustained value populations
        // represent temporal disagreement, not five individual outliers.
        // This is still a single regression, not a second LUT filter.
        if (residualOutliers > 0 && residualOutliers <= sparseLimit) {
            for (size_t i = 0; i < n; ++i) {
                if (weights[i] && residual[i] > limit) {
                    const float ratio = limit / residual[i];
                    weights[i] *= ratio * ratio;
                    if (ratio < .34f) { weights[i] = 0.0f; ++result.rejected; }
                }
            }
        }
    }
    // Final weighted ridge regression at target PTS. The coefficients below
    // are COMMON to the entire LUT, so we synthesize all texels only once.
    sum = first = second = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (weights[i] < 1e-5f) continue;
        sum += weights[i];
        first += weights[i] * seconds[i];
        second += weights[i] * seconds[i] * seconds[i];
        ++result.used;
        result.future += ordered[indices[i]].pts > target.pts;
    }
    if (result.used < 2 || sum <= 0) return result;
    // Less source-image change => stronger slope regularization. Genuine
    // changing lighting or a shot boundary is NOT globally brightness-clamped.
    const float change = ClampUnit((largestPictureShift - .008f) / .030f);
    const float ridge = sum * (.014f + (.001f - .014f) * change);
    const float denom = sum * (second + ridge) - first * first;
    std::array<float, MaxWindow> alpha{};
    if (denom > 1e-9f) {
        for (size_t i = 0; i < n; ++i)
            alpha[i] = weights[i] * ((second + ridge) - first * seconds[i]) / denom;
    } else {
        for (size_t i = 0; i < n; ++i) alpha[i] = weights[i] / sum;
    }
    // Keep the local-linear estimate within the convex hull of valid LUTs.
    // A one-sided window may otherwise extrapolate from old data and amplify
    // flicker. Move toward its weighted mean just enough to make all common
    // regression weights non-negative; never clip brightness or chroma.
    float relaxation = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (alpha[i] >= 0.0f) continue;
        const float mean = weights[i] / sum;
        if (mean > 0.0f) relaxation = (std::max)(relaxation, -alpha[i] / (mean - alpha[i]));
    }
    if (relaxation > 0.0f) {
        for (size_t i = 0; i < n; ++i)
            alpha[i] = (1.0f - relaxation) * alpha[i] + relaxation * weights[i] / sum;
    }
    // Cache-friendly, SIMD-friendly single synthesis: each source LUT and the
    // destination are traversed contiguously. This is the only full LUT pass.
    std::fill(output.begin(), output.end(), 0.0f);
    for (size_t i = 0; i < n; ++i) {
        if (weights[i] < 1e-5f || alpha[i] <= 0.0f) continue;
        const float* values = ordered[indices[i]].values;
        const float coefficient = alpha[i];
        result.contributions.terms[result.contributions.count++] = {ordered[indices[i]].pts, coefficient};
        for (size_t k = 0; k < output.size(); ++k)
            output[k] += coefficient * values[k];
    }
    result.blended = true;
    return result;
}
} // namespace svdlut::temporal
