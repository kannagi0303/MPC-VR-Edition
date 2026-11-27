// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr char DepthGpuShaders[] = R"(
Texture2D<float4> Capture : register(t0);
RWStructuredBuffer<float> Image : register(u0);
RWStructuredBuffer<float> Prediction : register(u1);
RWTexture2D<float> Depth : register(u2);
RWStructuredBuffer<uint> Range : register(u3);
cbuffer Settings : register(b0) {
    uint HistoryFlags; // bit 0: RGB cut history; bit 1: compatible W4 scale history
    uint ModelWidth;
    uint ModelHeight;
};

[numthreads(8, 8, 1)]
void Pack(uint2 pixel : SV_DispatchThreadID) {
    float3 rgb = Capture.Load(int3(pixel, 0)).rgb;
    uint i = pixel.y * ModelWidth + pixel.x;
    uint pixels = ModelWidth * ModelHeight;
    Image[i] = rgb.r;
    Image[i + pixels] = rgb.g;
    Image[i + 2 * pixels] = rgb.b;
}

groupshared uint4 SceneSums[256]; // RGB difference, brightening, darkening, chroma change
groupshared uint ScaleChange[256]; // sum of measured max-channel RGB changes (8-bit units)
static const uint SceneSampleCount = 1024;
static const uint TileRangeBase = 6;
static const uint LastRgbBase = TileRangeBase + 2 * 1024;

uint2 SamplePosition(uint i) {
    return (2 * uint2(i % 32, i / 32) + 1) * uint2(ModelWidth, ModelHeight) / 64;
}

uint PackRgb(float3 rgb) {
    uint3 b = uint3(round(saturate(rgb) * 255.0));
    return b.r | (b.g << 8) | (b.b << 16);
}

float3 UnpackRgb(uint packed) {
    return float3(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255) / 255.0;
}

bool FiniteDepth(float value) {
    return (asuint(value) & 0x7f800000u) != 0x7f800000u;
}

// Full-frame extrema still validate every inference. The scale reference is
// stored in the three previously unused words [3..5] of the existing Range
// buffer; no additional texture, dispatch, readback or model state.
// Keep a small pure-math boundary for native reference tests.
float RangeRetention(float lo, float hi, float oldLo, float oldHi, float rgbAgreement) {
    if (!(hi > lo && oldHi > oldLo) || !FiniteDepth(oldLo) || !FiniteDepth(oldHi) || oldLo < 0.0)
        return 0.0;
    const float overlap = max(0.0, min(hi, oldHi) - max(lo, oldLo));
    const float extent = max(hi, oldHi) - min(lo, oldLo);
    return extent > 0.0 ? saturate(rgbAgreement * (overlap / extent)) : 0.0;
}
float StabilizedDepth(float v, float lo, float hi, float refLo, float refHi, float keep) {
    // The raw map contributes whenever the reference differs from the current
    // range. Saturation of the reference alone cannot flatten new near/far
    // detail. Both terms are monotone in the CURRENT inverse-depth sample;
    // there is no pixel-wise averaging or reprojection of earlier maps.
    const float current = (v - lo) / (hi - lo);
    if (!(keep > 0.0 && refHi > refLo)) return 0.56 - 0.12 * current;
    const float reference = saturate((v - refLo) / (refHi - refLo));
    return 0.56 - 0.12 * saturate(lerp(current, reference, keep));
}

groupshared float2 RawExtrema[256];
static const uint RawTilePixels = 1024;

float2 RawEmpty() { return float2(asfloat(0x7f800000u), 0.0); }
float2 RawMerge(float2 a, float2 b) { return float2(min(a.x, b.x), max(a.y, b.y)); }
float2 RawSample(float v) {
    // Inverse depth is nonnegative; zero (infinite distance) is valid.
    return FiniteDepth(v) && v >= 0.0 ? float2(v, v) : float2(0.0, asfloat(0x7f800000u));
}
[numthreads(256, 1, 1)]
void RawRangeTiles(uint index : SV_GroupIndex, uint3 group : SV_GroupID) {
    const uint count = ModelWidth * ModelHeight;
    float2 bounds = RawEmpty();
    [unroll] for (uint i = 0; i < 4; ++i) {
        const uint pixel = group.x * RawTilePixels + index + i * 256;
        if (pixel < count) bounds = RawMerge(bounds, RawSample(Prediction[pixel]));
    }
    RawExtrema[index] = bounds;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) RawExtrema[index] = RawMerge(RawExtrema[index], RawExtrema[index + stride]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        uint oldBound;
        InterlockedExchange(Range[TileRangeBase + 2 * group.x], asuint(RawExtrema[0].x), oldBound);
        InterlockedExchange(Range[TileRangeBase + 2 * group.x + 1], asuint(RawExtrema[0].y), oldBound);
    }
}

[numthreads(256, 1, 1)]
void RawRange(uint index : SV_GroupIndex) {
    const uint tiles = (ModelWidth * ModelHeight + RawTilePixels - 1) / RawTilePixels;
    float2 bounds = RawEmpty();
    for (uint i = index; i < tiles; i += 256) {
        bounds = RawMerge(bounds, float2(asfloat(Range[TileRangeBase + 2 * i]),
            asfloat(Range[TileRangeBase + 2 * i + 1])));
    }
    RawExtrema[index] = bounds;
    uint4 scene = 0;
    uint scaleChange = 0;
    for (uint sampleIndex = index; sampleIndex < SceneSampleCount; sampleIndex += 256) {
        const float3 rgb = Capture.Load(int3(SamplePosition(sampleIndex), 0)).rgb;
        if (HistoryFlags & 1u) {
            const float3 change = rgb - UnpackRgb(Range[LastRgbBase + sampleIndex]);
            const float brightness = dot(change, float3(0.2126, 0.7152, 0.0722));
            const float3 chroma = abs(change - brightness);
            const float3 absoluteChange = abs(change);
            const float maxChange = max(absoluteChange.x, max(absoluteChange.y, absoluteChange.z));
            scaleChange += uint(round(saturate(maxChange) * 255.0));
            scene += uint4(maxChange > 0.20,
                brightness > 0.14, brightness < -0.14, max(chroma.x, max(chroma.y, chroma.z)) > 0.15);
        }
        Range[LastRgbBase + sampleIndex] = PackRgb(rgb);
    }
    SceneSums[index] = scene;
    ScaleChange[index] = scaleChange;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            RawExtrema[index] = RawMerge(RawExtrema[index], RawExtrema[index + stride]);
            SceneSums[index] += SceneSums[index + stride];
            ScaleChange[index] += ScaleChange[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        const float2 extrema = RawExtrema[0];
        const bool valid = FiniteDepth(extrema.x) && FiniteDepth(extrema.y) && extrema.y >= extrema.x;
        const uint4 sums = SceneSums[0];
        const bool cut = (HistoryFlags & 1u) && sums.x >= 740 &&
            ((sums.y >= 210 && sums.z >= 210) || sums.w >= 420);
        // Only reuse a range from the preceding accepted inference in the
        // same W4 sequence. The existing RGB cut decision is authoritative.
        float keep = 0.0;
        float2 reference = extrema;
        if (valid && extrema.y > extrema.x && !cut && (HistoryFlags & 2u)) {
            const float2 previous = float2(asfloat(Range[3]), asfloat(Range[4]));
            const float agreement = 1.0 - float(ScaleChange[0]) / (255.0 * SceneSampleCount);
            keep = RangeRetention(extrema.x, extrema.y, previous.x, previous.y, agreement);
            if (keep > 0.0) reference = lerp(extrema, previous, keep);
        }
        Range[3] = asuint(reference.x);
        Range[4] = asuint(reference.y);
        Range[5] = asuint(keep);
        uint oldBound;
        InterlockedExchange(Range[0], asuint(extrema.x), oldBound);
        InterlockedExchange(Range[1], asuint(extrema.y), oldBound);
        Range[2] = uint(valid) | (uint(cut) << 1);
    }
}

[numthreads(8, 8, 1)]
void RawNormalize(uint2 pixel : SV_DispatchThreadID) {
    const float v = Prediction[pixel.y * ModelWidth + pixel.x];
    const float lo = asfloat(Range[0]), hi = asfloat(Range[1]);
    // A flat, valid model output is neutral, not a failed inference. Retain
    // the common storage convention (near=0.44, far=0.56) for consumers.
    Depth[pixel] = (Range[2] & 1u) && hi > lo && FiniteDepth(v)
        ? StabilizedDepth(v, lo, hi, asfloat(Range[3]), asfloat(Range[4]), asfloat(Range[5])) : 0.5;
}
)";
