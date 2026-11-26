// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr char DepthGpuShaders[] = R"(
Texture2D<float4> Capture : register(t0);
RWStructuredBuffer<float> Image : register(u0);
RWStructuredBuffer<float> Prediction : register(u1);
RWTexture2D<float> Depth : register(u2);
RWStructuredBuffer<uint> Range : register(u3);
cbuffer Settings : register(b0) {
    uint HasPrevious;
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

// Full-frame extrema for this inference. RGB history detects scene cuts for W4.
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
    for (uint sampleIndex = index; sampleIndex < SceneSampleCount; sampleIndex += 256) {
        const float3 rgb = Capture.Load(int3(SamplePosition(sampleIndex), 0)).rgb;
        if (HasPrevious) {
            const float3 change = rgb - UnpackRgb(Range[LastRgbBase + sampleIndex]);
            const float brightness = dot(change, float3(0.2126, 0.7152, 0.0722));
            const float3 chroma = abs(change - brightness);
            const float3 absoluteChange = abs(change);
            scene += uint4(max(absoluteChange.x, max(absoluteChange.y, absoluteChange.z)) > 0.20,
                brightness > 0.14, brightness < -0.14, max(chroma.x, max(chroma.y, chroma.z)) > 0.15);
        }
        Range[LastRgbBase + sampleIndex] = PackRgb(rgb);
    }
    SceneSums[index] = scene;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            RawExtrema[index] = RawMerge(RawExtrema[index], RawExtrema[index + stride]);
            SceneSums[index] += SceneSums[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        const float2 extrema = RawExtrema[0];
        const bool valid = FiniteDepth(extrema.x) && FiniteDepth(extrema.y) && extrema.y >= extrema.x;
        const uint4 sums = SceneSums[0];
        const bool cut = HasPrevious && sums.x >= 740 &&
            ((sums.y >= 210 && sums.z >= 210) || sums.w >= 420);
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
        ? 0.56 - 0.12 * ((v - lo) / (hi - lo)) : 0.5;
}
)";
