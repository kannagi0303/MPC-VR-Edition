// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// P1: independent perceptual colour enhancement.
// The SAME pixel shader can process original SDR video (P1_STANDALONE),
// Normal A (P1_NORMAL_A), or RAW/Temporal B (P1_RAW_B). No extra drawing
// pass is needed when a LUT is already being rendered. Depth is optional.
// Colour-family tests are heuristics, NOT semantic object segmentation.
inline constexpr char PerceptualColorShader[] = R"(
Texture2D<float4> Picture : register(t0);
#if defined(P1_NORMAL_A) || defined(P1_RAW_B)
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
#endif
Texture2D<float> DepthColor : register(t2);
Texture2D<float> PreviousDepthColor : register(t3);
SamplerState DepthSampler : register(s2);
cbuffer Parameters : register(b0) {
    float4 Geometry;      // x,y: picture top-left; z,w: (width-1, height-1)
    float4 SourceControl; // x: Normal A grade blend; y,z: source offset; w: depth rotation
    float4 B21AndDepth;   // x: B2.1 luma participation; w: valid cached-depth confidence
    float4 Enhancements;  // x: Preferred Colors; y: Skin Tones; z: B2.1; w: adaptive B2.2
    float4 DepthTuning;   // x: previous available; y: previous rotation; z: max linear dim; w: final VE activation
    float4 OutputQuantization;
};
static const float3 YW = float3(.2126, .7152, .0722);
float Smooth(float a, float b, float x)
{
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
float2 BinUV(float2 p) { return (p * 32.0 + .5) / 33.0; }
float Headroom(float source, float delta)
{
    if (abs(delta) < 1e-6) return 1.0;
    return (delta > 0.0 ? 1.0 - source : source) / abs(delta);
}
#if defined(P1_NORMAL_A)
float3 ApplyNormalA(float3 source, float3 grade, float fade)
{
    float3 delta = grade - source;
    float3 chroma = delta - dot(delta, YW);
    float pressure = (max(max(delta.r, delta.g), delta.b) -
        min(min(delta.r, delta.g), delta.b)) * (255.0 / 18.0);
    float limit = min(Headroom(source.r, chroma.r),
        min(Headroom(source.g, chroma.g), Headroom(source.b, chroma.b)));
    float base = min(limit, rsqrt(1.0 + pressure * pressure));
    float strength = fade <= 1.0 ? fade * base : min(limit, fade * base);
    return source + chroma * strength;
}
#endif

// Proactive enhancement, rather than a hue-drift protection clamp.
// Preserve incoming brightness (B2.1's result if enabled); adapt chroma
// in the source's existing hue family without forcing any universal skin tone.
// [VE-05 | 2026-11-24 16:39:20 +08:00] Preferred colours (sky/foliage).
// [VE-06 | 2026-11-24 16:39:20 +08:00] Skin tones, jointly weighted with preferred colours.
float3 Enhance(float3 source, float3 grade, float depthWeight)
{
    float sy = dot(source, YW);
    float3 sc = source - sy;
    float cs = length(sc);
    if (sy <= .10 || sy >= .96 || cs <= .06 || cs >= .48) return grade;
    float3 axis = sc / max(cs, 1e-5);
    const float3 skinAxis = float3(.8153, -.1871, -.5479);
    const float3 foliageAxis = float3(-.6971, .2741, -.6625);
    const float3 skyAxis = float3(-.5990, .0978, .7947);
    float mid = Smooth(.11, .27, sy) * (1.0 - Smooth(.83, .96, sy));
    float moderate = Smooth(.055, .13, cs) * (1.0 - Smooth(.34, .48, cs));
    float weight = mid * moderate;
    // [VE-06] Source-family skin weight; no semantic segmentation.
    float skin = Enhancements.y * Smooth(.87, .985, dot(axis, skinAxis));
    // [VE-05] Source-family foliage/sky weights.
    float plant = Enhancements.x * Smooth(.86, .985, dot(axis, foliageAxis));
    float sky = Enhancements.x * Smooth(.87, .985, dot(axis, skyAxis));
    float selected = skin + plant + sky;
    if (selected < .002) return grade;

    float y = dot(grade, YW);
    float3 c = grade - y;
    float gc = length(c);
    if (gc <= .04) return grade;
    // Unrelated theatrical colour changes are not reinterpreted as daylight.
    float trust = Smooth(.45, .87, dot(c, axis) / max(gc, 1e-5));
    float w = saturate(selected * weight * trust * depthWeight);
    if (w <= .001) return grade;
    float3 preferred = skin * skinAxis + plant * foliageAxis + sky * skyAxis;
    preferred -= dot(preferred, YW);
    preferred *= rsqrt(max(dot(preferred, preferred), .0001));
    float hueBias = .045 * w;
    float3 adjusted = lerp(c, preferred * gc, hueBias);
    // Mildly boost selected chroma even when the source and grade already agree.
    // Tuned deliberately to be testable; the final release must be measured.
    float gain = 1.0 + .15 * w;
    adjusted *= gain;
    adjusted -= dot(adjusted, YW);
    // Reduce chroma only where it would exceed the output RGB gamut.
    float3 positive = max(adjusted, 0.0);
    float3 negative = max(-adjusted, 0.0);
    float hi = 1.0 - y;
    float lo = y;
    float fit = min(1.0, min(
        min(hi / max(positive.r, 1e-6), min(hi / max(positive.g, 1e-6), hi / max(positive.b, 1e-6))),
        min(lo / max(negative.r, 1e-6), min(lo / max(negative.g, 1e-6), lo / max(negative.b, 1e-6)))));
    return saturate(y + adjusted * saturate(fit));
}

float4 SvdLutGradeMain(float4 position)
{
    int2 p = int2(position.xy);
    int2 local = p - int2(Geometry.xy);
    float4 src = Picture.Load(int3(p + int2(SourceControl.yz), 0));
    if (any(local < 0) || any(local > int2(Geometry.zw))) return src;
    float3 original = saturate(src.rgb);
    float3 grade = original;
#if defined(P1_NORMAL_A) || defined(P1_RAW_B)
    uint edge = uint(local.x == int(Geometry.z)) + 2 * uint(local.y == int(Geometry.w));
    float layer = float(edge * 3);
    float3 rg = Lut.SampleLevel(LutSampler, float3(BinUV(original.rg), layer), 0).rgb;
    float3 rb = Lut.SampleLevel(LutSampler, float3(BinUV(original.rb), layer + 1), 0).rgb;
    float3 gb = Lut.SampleLevel(LutSampler, float3(BinUV(original.gb), layer + 2), 0).rgb;
#if defined(P1_NORMAL_A)
    grade = saturate(ApplyNormalA(original, rg + rb + gb, SourceControl.x));
#else
    grade = saturate(rg + rb + gb);
    if (Enhancements.z > .5) {
        grade = ApplyB21Luma(original, grade, B21AndDepth.x);
        if (Enhancements.w > .5)
            grade = ApplyB22Chroma(original, grade);
    }
    // B grade is mixed before perceptual enhancements: the latter stay independent.
    float b = saturate(SourceControl.x);
    grade = b >= 1.0 ? grade : lerp(original, grade, b);
#endif
#endif
    if (Enhancements.x <= .5 && Enhancements.y <= .5) return float4(grade, src.a);
    float depthWeight = 1.0;
    float farWeight = 0.0;
    if (B21AndDepth.w > .001) {
        float2 uv = (float2(local) + .5) / (Geometry.zw + 1.0);
        float2 sampleUV = SourceControl.w > .5 ? float2(1.0 - uv.y, uv.x) : uv;
        float currentDepth = DepthColor.SampleLevel(DepthSampler, sampleUV, 0);
        float depthValue = currentDepth;
        float reliability = 1.0;
        if (DepthTuning.x > .5) {
            // Temporally average ONLY when adjacent cached maps agree. If they
            // strongly disagree, attenuate both depth effects instead of
            // painting the jump into the movie. No motion-vector assumption.
            float2 priorUV = DepthTuning.y > .5 ? float2(1.0 - uv.y, uv.x) : uv;
            float previousDepth = PreviousDepthColor.SampleLevel(DepthSampler, priorUV, 0);
            float difference = abs(currentDepth - previousDepth);
            float stability = 1.0 - Smooth(.025, .14, difference);
            depthValue = lerp(currentDepth, .5 * (currentDepth + previousDepth), .6 * stability);
            reliability = lerp(.25, 1.0, stability);
        }
        // Retain D1's restrained depth modulation of Preferred/Skin colour.
        depthWeight = 1.0 + B21AndDepth.w * reliability * .12 *
            clamp((depthValue - .5) / .12, -1.0, 1.0);
        // Subtle Edition tuning: relatively distant areas are at most 1%
        // darker in linear light, falling back to zero without valid depth.
        // No artificial haze, sharpening or per-pixel depth inference.
        farWeight = Smooth(.50, .68, depthValue) *
            saturate(B21AndDepth.w * 2.0) * reliability;
    }
    float3 enhanced = Enhance(original, grade, depthWeight);
    if (farWeight > .001) {
        // Keep highlights nearly intact, avoiding conspicuous sky/lighting
        // shifts. First-order sRGB derivative approximates 1%-scale linear
        // luminance reduction without costly per-channel pow() transforms.
        float y = dot(enhanced, YW);
        float darken = min(DepthTuning.z, .02) * farWeight *
            (1.0 - Smooth(.80, .98, y));
        float3 srgbSlope = lerp(enhanced, (enhanced + .055) / 2.4,
            step(.04045, enhanced));
        enhanced = saturate(enhanced - darken * srgbSlope);
    }
    float activation = saturate(DepthTuning.w);
    return float4(activation >= 1.0 ? enhanced : lerp(original, enhanced, activation), src.a);
}
)";
