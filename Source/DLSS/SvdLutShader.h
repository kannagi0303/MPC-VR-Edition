// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SVDLUT in full-range sRGB, with the published spatial grid folded into the LUT.
inline constexpr char SvdLutShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);

cbuffer Parameters : register(b0) {
    float4 Geometry; // destination left, top, width minus one, height minus one
    float4 Transition; // grade weight, source offset x/y
};

float2 BinUV(float2 position)
{
    return (position * 32.0 + 0.5) / 33.0;
}

float ChannelHeadroom(float source, float delta)
{
    if (abs(delta) < 1e-6) return 1.0;
    return (delta > 0.0 ? 1.0 - source : source) / abs(delta);
}

float3 ApplyGrade(float3 source, float3 grade, float fade)
{
    const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
    // Remove the common brightness component before any gamut clipping.
    const float3 delta = grade - source;
    const float3 chroma = delta - dot(delta, lumaWeights);
    const float chromaDelta = max(max(delta.r, delta.g), delta.b) - min(min(delta.r, delta.g), delta.b);
    const float pressure = chromaDelta * (255.0 / 18.0);
    const float gamutLimit = min(ChannelHeadroom(source.r, chroma.r),
        min(ChannelHeadroom(source.g, chroma.g), ChannelHeadroom(source.b, chroma.b)));

    // One weight keeps the adjustment in gamut without changing source luma.
    const float strength = fade * min(gamutLimit, rsqrt(1.0 + pressure * pressure));
    return source + chroma * strength;
}

float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    const int2 outputPixel = int2(position.xy);
    const int2 pixel = outputPixel - int2(Geometry.xy);
    const float4 src = Picture.Load(int3(outputPixel + int2(Transition.yz), 0));
    if (any(pixel < 0) || any(pixel > int2(Geometry.zw)) || !any(src.rgb)) return src;

    const float3 rgb = saturate(src.rgb);
    // Interior, last column, last row, then the bottom-right pixel.
    const uint variant = uint(pixel.x == int(Geometry.z)) + 2 * uint(pixel.y == int(Geometry.w));
    const float layer = float(variant * 3);
    const float3 rg = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.rg), layer), 0).rgb;
    const float3 rb = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.rb), layer + 1), 0).rgb;
    const float3 gb = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.gb), layer + 2), 0).rgb;
    return float4(saturate(ApplyGrade(rgb, rg + rb + gb, Transition.x)), src.a);
}
)";


// Experiment B: direct published SVDLUT result, no A color guards or gain.
// Source/grade mapping matches the packed edge-variant layout of the model.
inline constexpr char SvdLutRawShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
cbuffer Parameters : register(b0) {
    float4 Geometry;
    float4 SourceControl; // x unused (100%), y,z source offset
};
float2 BinUV(float2 p) { return (p * 32.0 + 0.5) / 33.0; }
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    int2 p = int2(position.xy);
    int2 local = p - int2(Geometry.xy);
    float4 src = Picture.Load(int3(p + int2(SourceControl.yz), 0));
    if (any(local < 0) || any(local > int2(Geometry.zw))) return src;
    float3 rgb = saturate(src.rgb);
    uint edge = uint(local.x == int(Geometry.z)) + 2 * uint(local.y == int(Geometry.w));
    float layer = float(edge * 3);
    float3 rg = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.rg), layer), 0).rgb;
    float3 rb = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.rb), layer + 1), 0).rgb;
    float3 gb = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.gb), layer + 2), 0).rgb;
    return float4(saturate(rg + rb + gb), src.a);
}
)";

// B2.1 experiment: THREE fixed original-code-luma tone regions.
// LUT and temporal regression remain 100% B1. No second full-screen pass.
inline constexpr char SvdLutB2LuminanceShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
cbuffer Parameters : register(b0) {
    float4 Geometry;
    float4 SourceControl;
    float4 B2Luminance; // xyz = ORIGINAL-luma shadow/mid/high code gains
};
float2 BinUV(float2 p) { return (p * 32.0 + 0.5) / 33.0; }
float Smooth(float a, float b, float x)
{
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    int2 p = int2(position.xy);
    int2 local = p - int2(Geometry.xy);
    float4 src = Picture.Load(int3(p + int2(SourceControl.yz), 0));
    if (any(local < 0) || any(local > int2(Geometry.zw))) return src;
    float3 rgb = saturate(src.rgb);
    uint edge = uint(local.x == int(Geometry.z)) + 2 * uint(local.y == int(Geometry.w));
    float layer = float(edge * 3);
    float3 rg = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.rg), layer), 0).rgb;
    float3 rb = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.rb), layer + 1), 0).rgb;
    float3 gb = Lut.SampleLevel(LutSampler, float3(BinUV(rgb.gb), layer + 2), 0).rgb;
    float3 grade = saturate(rg + rb + gb);
    // Source-code luma is a shared, scene-INDEPENDENT anchor; never re-rank
    // each scene's histogram into arbitrary shadow/highlight regions.
    float sourceY = dot(rgb, float3(.2126, .7152, .0722));
    float a = Smooth(.12, .52, sourceY);
    float b = Smooth(.52, .90, sourceY);
    float gain = lerp(lerp(B2Luminance.x, B2Luminance.y, a), B2Luminance.z, b);
    if (gain > 1.0)
        gain = min(gain, 1.0 / max(max(grade.r, grade.g), max(grade.b, 0.0001)));
    return float4(saturate(grade * gain), src.a);
}
)";


// B2.2: general-purpose SDR gamut mapping + restrained preferred memory colours.
// The SVDLUT floating-point result is kept UNCLIPPED until the last return.
// The B1 model, temporal regression, and standalone B2.1 shader are untouched.
// One existing full-resolution pass, no extra inference or GPU readback.
// Color-family weights are soft heuristics, NOT scene/object recognition.
inline constexpr char SvdLutB2MemoryShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
cbuffer Parameters : register(b0) {
    float4 Geometry;
    float4 SourceControl;
    float4 B2Luminance; // B2.1's existing zonal gains, or identity (1,1,1)
};
static const float3 YW = float3(.2126, .7152, .0722);
float2 BinUV(float2 p) { return (p * 32.0 + 0.5) / 33.0; }
float Smooth(float a, float b, float x)
{
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

// A very small, source-guided preference for plausible memory-colour ranges.
// Other hues (including saturated stage LEDs) are left unchanged by this step.
// Keep weighted code-luma unchanged; the universal gamut mapping follows.
float3 PreferredMemoryColor(float3 original, float3 graded)
{
    float sourceY = dot(original, YW);
    if (sourceY < .14 || sourceY > .93) return graded;
    float3 sourceC = original - sourceY;
    float sourceC2 = dot(sourceC, sourceC);
    if (sourceC2 < .0049 || sourceC2 > .25) return graded;
    float sourceSize = sqrt(sourceC2);
    float3 sourceAxis = sourceC / sourceSize;

    // These are *candidate* directions, not semantic skin/plant/sky masks.
    const float3 skinAxis = float3(.8153, -.1871, -.5479);
    const float3 foliageAxis = float3(-.6971, .2741, -.6625);
    const float3 skyAxis = float3(-.5990, .0978, .7947);
    float skin = Smooth(.87, .98, dot(sourceAxis, skinAxis));
    float foliage = Smooth(.86, .98, dot(sourceAxis, foliageAxis));
    float sky = Smooth(.87, .98, dot(sourceAxis, skyAxis));
    float weightSum = skin + foliage + sky;
    if (weightSum < .05) return graded;
    float confidence = Smooth(.065, .14, sourceSize) *
        (1.0 - Smooth(.33, .48, sourceSize)) *
        Smooth(.14, .29, sourceY) * (1.0 - Smooth(.78, .93, sourceY));
    if (confidence < .01) return graded;

    float gradedY = dot(graded, YW);
    float3 gradedC = graded - gradedY;
    float gradedC2 = dot(gradedC, gradedC);
    if (gradedC2 < .0025) return graded;
    float gradedSize = sqrt(gradedC2);
    // Do not reinterpret a deliberate change to an unrelated lighting hue.
    float directionTrust = Smooth(.55, .88,
        dot(gradedC, sourceAxis) / gradedSize);
    float select = confidence * directionTrust * saturate(weightSum);
    if (select < .005) return graded;

    float3 preferred = (skin * skinAxis + foliage * foliageAxis + sky * skyAxis) /
        weightSum;
    preferred -= dot(preferred, YW); // strictly preserve coded luma
    preferred *= rsqrt(max(dot(preferred, preferred), .0001));
    // A gentle proactive hue preference, never a forced neutral/daylight skin.
    float hueWeight = .075 * select;
    float3 chroma = lerp(gradedC, preferred * gradedSize, hueWeight);
    // Restore a little colour only when B1 has desaturated a plausible
    // memory-colour candidate relative to the original source.
    float referenceSize = sourceSize * clamp(gradedY / max(sourceY, .14), .80, 1.20);
    float restore = min(.025, max(0.0, referenceSize - length(chroma))) * .40 * select;
    chroma *= 1.0 + restore / max(length(chroma), .05);
    return gradedY + chroma;
}

// B2.2 gamut mapping with a B2.1 luminance anchor and a strictly progressive
// radial chroma roll-off. Avoid boundary projection: projecting every
// out-of-range chroma vector to the same cube face flattens coloured texture.
// All rules are hue-independent; no concert/skin/foliage special cases here.
float3 MapToSdrGamut(float3 original, float3 graded, float3 b21Reference)
{
    float sourceY = dot(original, YW);
    float referenceY = dot(b21Reference, YW);
    float y = dot(graded, YW);
    float3 c = graded - y;
    float3 sourceC = original - sourceY;

    // In a strongly over-range pixel, an otherwise flat B1/B2.1 reference
    // may have lost its shading. Retain a small part of the ORIGINAL pixel's
    // encoded-luma variation, without adding neighbouring texture reads.
    float sourceSize = sqrt(dot(sourceC, sourceC));
    float gradeSize = sqrt(dot(c, c));
    float growth = gradeSize / max(sourceSize, .10);
    float overshoot = max(max(max(-graded.r, graded.r - 1.0),
                             max(-graded.g, graded.g - 1.0)),
                             max(-graded.b, graded.b - 1.0));
    float textureRisk = Smooth(.02, .28, overshoot) * Smooth(1.10, 1.75, growth);
    float textureShift = clamp((sourceY - referenceY) * (.30 * textureRisk),
                               -.055, .055);
    float targetY = referenceY + textureShift;

    // Preserve untouched legal colours without reshaping contrast.
    float3 targetRgb = targetY + c;
#if defined(SVDLUT_R1_CONTINUOUS)
    // R1: a mathematically exact no-op region for knee=.98. Preserve the
    // common in-gamut fast path without a hard grade/source ratio threshold.
    // With textureRisk=0, a 0.02 channel margin guarantees radius<=.98.
    if (textureRisk <= 0.0 && all(targetRgb >= .02) && all(targetRgb <= .98))
        return targetRgb;
#else
    if (all(targetRgb >= 0.0) && all(targetRgb <= 1.0) &&
        gradeSize <= 1.04 * max(sourceSize, .085)) return targetRgb;
#endif

    // Distance to the SDR RGB cube boundary along the *single* chroma ray.
    float3 pos = max(c, 0.0);
    float3 neg = max(-c, 0.0);
    float upper = 1.0 - targetY;
    float lower = targetY;
    float maxScale = min(
        min(upper / max(pos.r, 1e-6),
            min(upper / max(pos.g, 1e-6), upper / max(pos.b, 1e-6))),
        min(lower / max(neg.r, 1e-6),
            min(lower / max(neg.g, 1e-6), lower / max(neg.b, 1e-6))));
    maxScale = max(0.0, maxScale);

    // A rational soft knee maps any finite overshoot INSIDE the RGB cube.
    // Unlike min(1, maxScale), the output stays ordered as input chroma grows:
    // no abrupt plateau on the boundary, and gentle behaviour inside gamut.
    float radius = 1.0 / max(maxScale, 1e-6);
#if defined(SVDLUT_R1_CONTINUOUS)
    // Screening value, not claimed as libplacebo or ACES equivalence.
    const float knee = .98;
#else
    const float knee = .92;
#endif
    float compressedRadius = radius;
    if (radius > knee) {
        float over = radius - knee;
        compressedRadius = knee + (1.0 - knee) * over / (1.0 - knee + over);
    }
    float scale = compressedRadius / max(radius, 1e-6);
    float3 mapped = saturate(targetY + c * scale);

#if defined(SVDLUT_R1_CONTINUOUS)
    // Risk fades continuously to zero; do not introduce a .005 jump.
    if (textureRisk <= 0.0) return mapped;
#else
    if (textureRisk < .005) return mapped; // common-case: no source-detail blend
#endif

    // Pure gamut compression cannot recreate original tonal differences once
    // the LUT drives many source colours into the same saturated region.
    // In that exceptional case, retain some of the original per-pixel colour
    // variation at the B2.1 brightness anchor. This is NOT spatial sharpening
    // and requires no neighbouring image reads or additional pass.
    float positiveSource = max(max(sourceC.r, sourceC.g), max(sourceC.b, 0.0));
    float negativeSource = max(max(-sourceC.r, -sourceC.g), max(-sourceC.b, 0.0));
    float sourceFit = min(1.0, min((1.0 - targetY) / max(positiveSource, 1e-6),
                                   targetY / max(negativeSource, 1e-6)));
    float3 sourceShading = targetY + sourceC * saturate(sourceFit);
    return saturate(lerp(mapped, sourceShading, .30 * textureRisk));
}

float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    int2 p = int2(position.xy);
    int2 local = p - int2(Geometry.xy);
    float4 src = Picture.Load(int3(p + int2(SourceControl.yz), 0));
    if (any(local < 0) || any(local > int2(Geometry.zw))) return src;
    float3 original = saturate(src.rgb); // LUT input indices only
    uint edge = uint(local.x == int(Geometry.z)) + 2 * uint(local.y == int(Geometry.w));
    float layer = float(edge * 3);
    float3 rg = Lut.SampleLevel(LutSampler, float3(BinUV(original.rg), layer), 0).rgb;
    float3 rb = Lut.SampleLevel(LutSampler, float3(BinUV(original.rb), layer + 1), 0).rgb;
    float3 gb = Lut.SampleLevel(LutSampler, float3(BinUV(original.gb), layer + 2), 0).rgb;
    float3 publishedGrade = rg + rb + gb; // keep the full float LUT

    // Identical B2.1 zonal-gain *formula*; no premature headroom clipping.
    // B2.1 standalone uses its original shader with exact prior behaviour.
    float sourceY = dot(original, YW);
    float a = Smooth(.12, .52, sourceY);
    float b = Smooth(.52, .90, sourceY);
    float gain = lerp(lerp(B2Luminance.x, B2Luminance.y, a), B2Luminance.z, b);
    // B2.2 should not create a NEW exposure intent from out-of-range LUT
    // values: anchor its luminance to the already established B2.1 result.
    // With B2.1 disabled, this becomes the existing B1 SDR output.
    float3 reference = saturate(publishedGrade);
    float referenceGain = gain;
    if (referenceGain > 1.0)
        referenceGain = min(referenceGain,
            1.0 / max(max(reference.r, reference.g), max(reference.b, .0001)));
    reference = saturate(reference * referenceGain);

    float3 grade = PreferredMemoryColor(original, publishedGrade * gain);
    return float4(MapToSdrGamut(original, grade, reference), src.a);
}
)";
