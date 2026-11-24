// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SVDLUT model preprocessing: explicit FP32 bilinear with half-pixel centers,
// clamped to the selected picture, not the containing texture's black borders.
inline constexpr char SvdLutThumbnailShader[] = R"(
Texture2D<float4> Picture : register(t0);
cbuffer InputRect : register(b0) { float4 Rect; }; // left, top, width, height
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    float2 p = clamp(position.xy * Rect.zw / 256.0 - 0.5, 0.0, Rect.zw - 1.0);
    int2 lo = int2(floor(p));
    int2 hi = min(lo + 1, int2(Rect.zw) - 1);
    float2 w = p - float2(lo);
    int2 origin = int2(Rect.xy);
    float3 a = Picture.Load(int3(origin + lo, 0)).rgb;
    float3 b = Picture.Load(int3(origin + int2(hi.x, lo.y), 0)).rgb;
    float3 c = Picture.Load(int3(origin + int2(lo.x, hi.y), 0)).rgb;
    float3 d = Picture.Load(int3(origin + hi, 0)).rgb;
    return float4(saturate(lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y)), 1.0);
}
)";

// B2.1 SetLum/ClipColor-style gamut-safe code-luma replacement.
// A participation of 1 is the existing, completely unmodified B1 path.
inline constexpr char SvdLutAdaptiveLumaShader[] = R"(
float3 ApplyB21Luma(float3 source, float3 grade, float participation)
{
    if (participation >= 1.0) return grade;
    const float3 weights = float3(.2126, .7152, .0722);
    float sy = dot(source, weights);
    float gy = dot(grade, weights);
    float y = saturate(sy + clamp(saturate(participation) * (gy-sy), -4.0/255.0, 4.0/255.0));
    float3 c = grade - gy;
    float hi = max(max(c.r,c.g),max(c.b,1e-6));
    float lo = max(max(-c.r,-c.g),max(-c.b,1e-6));
    float fit = saturate(min((1.0-y)/hi,y/lo));
    return saturate(y + c * fit);
}

// B2.2 candidate: soft-limit only the proposed chroma displacement from
// the source at B2.1's output Y'. ACES-style rational knee, NOT ACES gamut
// mapping. Both endpoints are already in gamut at the same Y', so their
// convex combination preserves that anchor without per-channel clipping.
float3 ApplyB22Chroma(float3 source, float3 calibrated)
{
    const float3 weights = float3(.2126, .7152, .0722);
    float sy = dot(source, weights);
    float y = saturate(dot(calibrated, weights));
    float3 sc = source - sy;
    float hi = max(max(sc.r,sc.g),max(sc.b,1e-6));
    float lo = max(max(-sc.r,-sc.g),max(-sc.b,1e-6));
    float fit = saturate(min((1.0-y)/hi,y/lo));
    float3 anchor = y + sc * fit;
    float3 delta = calibrated - anchor;
    float3 ad = abs(delta);
    float radius = max(max(ad.r,ad.g),ad.b);
    float3 ac = abs(sc);
    float sourceRadius = max(max(ac.r,ac.g),ac.b);
    // Screening values: near-neutral tint allowance of two code values,
    // increasingly permissive on colourful pixels. Not a JND or standard.
    float limit = 2.0/255.0 + 1.5 * sourceRadius;
    float knee = .65 * limit;
    if (radius <= knee) return calibrated;
    float span = limit - knee;
    float excess = radius - knee;
    float compressed = knee + span * excess / (span + excess);
    return saturate(anchor + delta * (compressed / radius));
}
)";

// SVDLUT in full-range sRGB, with the published spatial grid folded into the LUT.
inline constexpr char SvdLutShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);

cbuffer Parameters : register(b0) {
    float4 Geometry; // destination left, top, width minus one, height minus one
    float4 Transition; // grade weight, source offset x/y
    float4 OutputQuantization;
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
    const float base = min(gamutLimit, rsqrt(1.0 + pressure * pressure));
    const float strength = fade <= 1.0 ? fade * base :
        min(gamutLimit, fade * base); // >100% cannot exceed gamut headroom
    return source + chroma * strength;
}

float4 SvdLutGradeMain(float4 position)
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
    float4 SourceControl; // x grade blend, y,z source offset
    float4 UnusedB2; // same register layout as the other B shaders
    float4 OutputQuantization;
};
float2 BinUV(float2 p) { return (p * 32.0 + 0.5) / 33.0; }
float4 SvdLutGradeMain(float4 position)
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
    float3 full = saturate(rg + rb + gb);
    float t = saturate(SourceControl.x);
    return float4(t >= 1.0 ? full : lerp(rgb, full, t), src.a);
}
)";

// B2.1: source-adaptive independent code-luma participation.
// LUT and temporal regression remain 100% B1. No second full-screen pass.
inline constexpr char SvdLutB2LuminanceShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
cbuffer Parameters : register(b0) {
    float4 Geometry;
    float4 SourceControl;
    float4 B2Luminance; // x = luma participation; yz reserved compatibility
    float4 OutputQuantization;
};
float2 BinUV(float2 p) { return (p * 32.0 + 0.5) / 33.0; }
float Smooth(float a, float b, float x)
{
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
float4 SvdLutGradeMain(float4 position)
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
    float3 full = ApplyB21Luma(rgb, grade, B2Luminance.x);
    float t = saturate(SourceControl.x);
    return float4(t >= 1.0 ? full : lerp(rgb, full, t), src.a);
}
)";


// B2.2: general-purpose SDR gamut mapping + restrained preferred memory colours.
// The SVDLUT floating-point result is kept UNCLIPPED until the last return.
// The B1 model and temporal regression remain untouched.
// One existing full-resolution pass, no extra inference or GPU readback.
// Color-family weights are soft heuristics, NOT scene/object recognition.
inline constexpr char SvdLutB2MemoryShader[] = R"(
Texture2D<float4> Picture : register(t0);
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
cbuffer Parameters : register(b0) {
    float4 Geometry;
    float4 SourceControl;
    float4 B2Luminance; // x=luma participation, w=D1 depth strength (M1 only)
    float4 OutputQuantization;
};
#if defined(SVDLUT_M1_SKIN_GUARD)
Texture2D<float> DepthColor : register(t2);
SamplerState DepthSampler : register(s2);
#endif
static const float3 YW = float3(.2126, .7152, .0722);
float2 BinUV(float2 p) { return (p * 32.0 + 0.5) / 33.0; }
float Smooth(float a, float b, float x)
{
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

#if defined(SVDLUT_M1_SKIN_GUARD)
// M2 fusion: one shared source analysis for existing B2.2 memory preference,
// the opt-in M1 skin guard, and the existing B2.2/R1 gamut mapper.
// The 3 x 2D pairwise LUTs and a single display-size draw stay unchanged.
// This metadata is GPU register data, not a new texture or readback.
struct PerceptualSource {
    float y;
    float3 chroma;
    float chroma2;
    float size;
    float3 axis;
    float skinAlignment;
};
PerceptualSource AnalyzePerceptualSource(float3 rgb, float y)
{
    PerceptualSource s;
    s.y = y; // Already calculated for the B2.1 tone-zone interpolation.
    s.chroma = rgb - y;
    s.chroma2 = dot(s.chroma, s.chroma);
    s.size = sqrt(s.chroma2); // The gamut mapper needs it even for non-skin pixels.
    s.axis = 0.0;
    s.skinAlignment = 0.0;
    // A/B2.2/R1 with M1 OFF retains its original, separate source code.
    if (y >= .14 && y <= .94 && s.chroma2 >= .0049 && s.chroma2 <= .25) {
        s.axis = s.chroma / max(s.size, 1e-6);
        s.skinAlignment = dot(s.axis, float3(.8153, -.1871, -.5479));
    }
    return s;
}
#endif

// A very small, source-guided preference for plausible memory-colour ranges.
// Other hues (including saturated stage LEDs) are left unchanged by this step.
// Keep weighted code-luma unchanged; the universal gamut mapping follows.
float3 PreferredMemoryColor(float3 original, float3 graded
#if defined(SVDLUT_M1_SKIN_GUARD)
    , PerceptualSource s, float depthWeight
#endif
)
{
#if defined(SVDLUT_M1_SKIN_GUARD)
    float sourceY = s.y;
    if (sourceY < .14 || sourceY > .93) return graded;
    float sourceC2 = s.chroma2;
    if (sourceC2 < .0049 || sourceC2 > .25) return graded;
    float sourceSize = s.size;
    float3 sourceAxis = s.axis;
#else
    float sourceY = dot(original, YW);
    if (sourceY < .14 || sourceY > .93) return graded;
    float3 sourceC = original - sourceY;
    float sourceC2 = dot(sourceC, sourceC);
    if (sourceC2 < .0049 || sourceC2 > .25) return graded;
    float sourceSize = sqrt(sourceC2);
    float3 sourceAxis = sourceC / sourceSize;
#endif

    // These are *candidate* directions, not semantic skin/plant/sky masks.
    const float3 skinAxis = float3(.8153, -.1871, -.5479);
    const float3 foliageAxis = float3(-.6971, .2741, -.6625);
    const float3 skyAxis = float3(-.5990, .0978, .7947);
#if defined(SVDLUT_M1_SKIN_GUARD)
    float skin = Smooth(.87, .98, s.skinAlignment);
#else
    float skin = Smooth(.87, .98, dot(sourceAxis, skinAxis));
#endif
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
#if defined(SVDLUT_M1_SKIN_GUARD)
    hueWeight *= depthWeight; // D1 changes strength, not hue target/exposure
#endif
    float3 chroma = lerp(gradedC, preferred * gradedSize, hueWeight);
    // Restore a little colour only when B1 has desaturated a plausible
    // memory-colour candidate relative to the original source.
    float referenceSize = sourceSize * clamp(gradedY / max(sourceY, .14), .80, 1.20);
    float restore = min(.025, max(0.0, referenceSize - length(chroma))) * .40 * select;
    chroma *= 1.0 + restore / max(length(chroma), .05);
    return gradedY + chroma;
}

// M1: Opt-in source-referenced SKIN-HUE GUARD on top of existing B2.2/R1.
// Inspired by RawTherapee's hue/chroma/lightness gating and ColorRestore's
// soft weights, but implemented independently for the current SVDLUT shader.
// This is a colour-family heuristic, NOT face segmentation or an absolute
// skin-colour target. Leave intentional strong lighting changes alone.
#if defined(SVDLUT_M1_SKIN_GUARD)
float3 GuardSkinHue(PerceptualSource s, float3 graded, float depthWeight)
{
    const float sourceY = s.y;
    if (sourceY <= .14 || sourceY >= .94) return graded;
    const float sourceC2 = s.chroma2;
    // Exclude grey/near-black, extremely vivid lighting and other outliers.
    if (sourceC2 <= .0049 || sourceC2 >= .20) return graded;
    const float3 sourceAxis = s.axis;
    const float skin = Smooth(.86, .98, s.skinAlignment);
    const float lightness = Smooth(.14, .30, sourceY) *
        (1.0 - Smooth(.79, .94, sourceY));
    const float chromaBand = Smooth(.0049, .015, sourceC2) *
        (1.0 - Smooth(.13, .20, sourceC2));
    const float confidence = skin * lightness * chromaBand;
    if (confidence <= .01) return graded;

    const float gradeY = dot(graded, YW);
    const float3 gradeC = graded - gradeY;
    const float gradeC2 = dot(gradeC, gradeC);
    if (gradeC2 <= .0025) return graded;
    const float agreement = dot(sourceAxis, gradeC) * rsqrt(gradeC2);
    // Moderate SVDLUT hue drift: small correction. A complete hue change
    // (e.g. theatrical blue/red lighting) is NOT treated as a defect.
    const float drift = 1.0 - Smooth(.91, .995, agreement);
    const float lightingTrust = Smooth(.42, .72, agreement);
    const float blend = .30 * confidence * drift * lightingTrust * depthWeight;
    if (blend <= .001) return graded;
    const float3 keptHue = sourceAxis * sqrt(gradeC2);
    float3 chroma = lerp(gradeC, keptHue, blend);
    // Keep the graded luma exactly; do not introduce a brightness correction.
    chroma -= dot(chroma, YW);
    return gradeY + chroma;
}
#endif

// B2.2 gamut mapping with a B2.1 luminance anchor and a strictly progressive
// radial chroma roll-off. Avoid boundary projection: projecting every
// out-of-range chroma vector to the same cube face flattens coloured texture.
// All rules are hue-independent; no concert/skin/foliage special cases here.
float3 MapToSdrGamut(float3 original, float3 graded, float3 b21Reference
#if defined(SVDLUT_M1_SKIN_GUARD)
    , PerceptualSource s
#endif
)
{
#if defined(SVDLUT_M1_SKIN_GUARD)
    float sourceY = s.y;
#else
    float sourceY = dot(original, YW);
#endif
    float referenceY = dot(b21Reference, YW);
    float y = dot(graded, YW);
    float3 c = graded - y;
#if defined(SVDLUT_M1_SKIN_GUARD)
    float3 sourceC = s.chroma;
#else
    float3 sourceC = original - sourceY;
#endif

    // In a strongly over-range pixel, an otherwise flat B1/B2.1 reference
    // may have lost its shading. Retain a small part of the ORIGINAL pixel's
    // encoded-luma variation, without adding neighbouring texture reads.
#if defined(SVDLUT_M1_SKIN_GUARD)
    float sourceSize = s.size;
#else
    float sourceSize = sqrt(dot(sourceC, sourceC));
#endif
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

float4 SvdLutGradeMain(float4 position)
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

    float sourceY = dot(original, YW);
    // B2.1 off: keep the original unbounded memory/gamut path exactly.
    // B2.1 on: use the calibrated SDR candidate and its code-luma anchor.
    float3 reference = ApplyB21Luma(original, saturate(publishedGrade), B2Luminance.x);
    float3 candidate = B2Luminance.x >= 1.0 ? publishedGrade : reference;

#if defined(SVDLUT_M1_SKIN_GUARD)
    // Reuse the source's B2.1 luma and ONE chroma/skin-family analysis across
    // memory preference, M1 and gamut mapping. No extra draw, texture, or model.
    PerceptualSource s = AnalyzePerceptualSource(original, sourceY);
    float depthWeight = 1.0;
    // Uniform branch: when D1 is inactive, no depth texture is sampled.
    // D1 only changes already-selected memory/skin hue guards, NOT the
    // learnt SVDLUT grade, brightness, or a scene's intended lighting.
    if (B2Luminance.w > .001) {
        const float2 uv = (float2(local) + .5) / (Geometry.zw + 1.0);
        const float2 sampleUV = SourceControl.w > .5 ? float2(1.0 - uv.y, uv.x) : uv;
        const float signedDepth = clamp((DepthColor.SampleLevel(DepthSampler, sampleUV, 0) - .5) / .06, -1.0, 1.0);
        // Depth is only relative; it is never interpreted as metric distance.
        // Restrained +/-30% strength modulation avoids visible colour seams.
        depthWeight = 1.0 + B2Luminance.w * .30 * signedDepth;
    }
    float3 grade = PreferredMemoryColor(original, candidate, s, depthWeight);
    grade = GuardSkinHue(s, grade, depthWeight);
    float3 full = MapToSdrGamut(original, grade, reference, s);
    float t = saturate(SourceControl.x);
    return float4(t >= 1.0 ? full : lerp(original, full, t), src.a);
#else
#if !defined(SVDLUT_R1_CONTINUOUS)
    // Only the plain B2.2 + B2.1 experiment changes. B2.1 off, R1 and M1
    // retain their previous independent/legacy behaviour.
    if (B2Luminance.x < 1.0) {
        float3 calibrated = ApplyB22Chroma(original, reference);
        float blend = saturate(SourceControl.x);
        return float4(blend >= 1.0 ? calibrated : lerp(original, calibrated, blend), src.a);
    }
#endif
    float3 grade = PreferredMemoryColor(original, candidate);
    float3 full = MapToSdrGamut(original, grade, reference);
    float t = saturate(SourceControl.x);
    return float4(t >= 1.0 ? full : lerp(original, full, t), src.a);
#endif
}
)";

// Appended to every SVDLUT grade variant, after its independent color math.
// Reuse MPC-VR's ordered threshold texture; never dither an already integral
// output code or black borders. Quantization is at THIS UNORM write, not a
// claim that any earlier MFG/VP/backbuffer precision can be recovered.
inline constexpr char SvdLutOutputShader[] = R"(
Texture2D<float4> OutputDither : register(t4);
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    float4 result = SvdLutGradeMain(position);
    int2 p = int2(position.xy);
    int2 local = p - int2(Geometry.xy);
    float q = OutputQuantization.x;
    if (q > 0.0 && all(local >= 0) && all(local <= int2(Geometry.zw))) {
        float3 code = saturate(result.rgb) * q;
        float3 nearest = round(code);
        float threshold = min(OutputDither.Load(int3(p.x & 31, p.y & 31, 0)).r, 0.999999);
        float3 dithered = floor(code + threshold);
        // Avoid a one-code change from floating point roundoff at integers.
        result.rgb = float3(
            abs(code.r - nearest.r) < 0.0002 ? nearest.r : dithered.r,
            abs(code.g - nearest.g) < 0.0002 ? nearest.g : dithered.g,
            abs(code.b - nearest.b) < 0.0002 ? nearest.b : dithered.b) / q;
    }
    return result;
}
)";
