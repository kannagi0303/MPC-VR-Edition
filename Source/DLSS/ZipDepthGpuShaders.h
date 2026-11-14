// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr char ZipDepthGpuShaders[] = R"(
Texture2D<float4> Capture : register(t0);
RWStructuredBuffer<float> Image : register(u0);
RWStructuredBuffer<float> Prediction : register(u1);
RWTexture2D<float> Depth : register(u2);
RWStructuredBuffer<uint> Range : register(u3);
cbuffer Settings : register(b0) {
    uint Width;
    uint Height;
    uint HasPrevious;
};
static const uint Side = 384;

[numthreads(8, 8, 1)]
void Pack(uint2 pixel : SV_DispatchThreadID) {
    float3 rgb = Capture.Load(int3(pixel, 0)).rgb;
    uint i = pixel.y * Side + pixel.x;
    Image[i] = rgb.r;
    Image[i + Side * Side] = rgb.g;
    Image[i + 2 * Side * Side] = rgb.b;
}

groupshared float Samples[1024];
groupshared float2 StableBounds;
groupshared float4 AnchorSums[256]; // current, previous, current², previous²
groupshared float2 AnchorCross[256]; // current * previous, count
static const uint AnchorBase = 3;
static const uint AnchorCount = 1024;

uint2 SamplePosition(uint i) {
    const uint2 size = uint2(Width, Height);
    return (Side - size) / 2 + (2 * uint2(i % 32, i / 32) + 1) * size / 64;
}

uint PackRgb(float3 rgb) {
    uint3 b = uint3(round(saturate(rgb) * 255.0));
    return b.r | (b.g << 8) | (b.b << 16);
}

float3 UnpackRgb(uint packed) {
    return float3(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255) / 255.0;
}

// Sort a 32x32 grid from the picture area, excluding replicated padding.
// Run this once per estimate, not for each pixel of the depth texture.
[numthreads(256, 1, 1)]
void Percentiles(uint index : SV_GroupIndex) {
    for (uint i = index; i < AnchorCount; i += 256) {
        uint2 p = SamplePosition(i);
        float v = Prediction[p.y * Side + p.x];
        Samples[i] = isfinite(v) ? v : asfloat(0x7f800000u);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint length = 2; length <= 1024; length <<= 1) {
        for (uint step = length >> 1; step > 0; step >>= 1) {
            // Each lane owns two disjoint compare-and-swap pairs per pass.
            for (uint pair = index; pair < 512; pair += 256) {
                uint first = (pair / step) * (2 * step) + pair % step;
                uint second = first + step;
                float a = Samples[first], b = Samples[second];
                bool ascending = (first & length) == 0;
                Samples[first] = ascending ? min(a, b) : max(a, b);
                Samples[second] = ascending ? max(a, b) : min(a, b);
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    const float lo = Samples[102], hi = Samples[921];
    const bool valid = isfinite(Samples[1023]) && hi > lo + 1e-5;
    // Compare sparse points at the same location when their source RGB still
    // matches. Fit only a global affine mapping so moving objects can change depth.
    float4 sums = 0.0;
    float2 cross = 0.0;
    for (uint i = index; valid && i < AnchorCount; i += 256) {
        uint2 p = SamplePosition(i);
        float3 rgb = Capture.Load(int3(p, 0)).rgb;
        float raw = Prediction[p.y * Side + p.x];
        if (HasPrevious) {
            float3 delta = abs(rgb - UnpackRgb(Range[AnchorBase + 2 * i]));
            float prior = asfloat(Range[AnchorBase + 2 * i + 1]);
            // Exclude black bars and very dark areas as scene anchors.
            if (max(max(rgb.r, rgb.g), rgb.b) > 0.06 &&
                max(max(delta.r, delta.g), delta.b) < 0.04) {
                float x = saturate((raw - lo) / (hi - lo));
                sums += float4(x, prior, x * x, prior * prior);
                cross += float2(x * prior, 1.0);
            }
        }
        Range[AnchorBase + 2 * i] = PackRgb(rgb);
    }
    AnchorSums[index] = sums;
    AnchorCross[index] = cross;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            AnchorSums[index] += AnchorSums[index + stride];
            AnchorCross[index] += AnchorCross[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        float2 bounds = float2(lo, hi);
        float count = AnchorCross[0].y;
        if (valid && count >= 192.0) {
            float4 mean = AnchorSums[0] / count;
            float cov = AnchorCross[0].x / count - mean.x * mean.y;
            float varX = max(0.0, mean.z - mean.x * mean.x);
            float varY = max(0.0, mean.w - mean.y * mean.y);
            // A fit with little depth variation only supports offset correction.
            // Reject a poorly correlated fit when the anchors have real spread.
            bool flat = max(varX, varY) < 0.005;
            bool correlated = min(varX, varY) >= 0.005 && cov > 0.0 &&
                cov * cov >= 0.7 * varX * varY;
            if (flat || correlated) {
                float gain = correlated ? clamp(cov / varX, 0.65, 1.55) : 1.0;
                float offset = clamp(mean.y - gain * mean.x, -0.35, 0.35);
                float2 aligned = lo + (float2(0.0, 1.0) - offset) * ((hi - lo) / gain);
                // Slowly return toward the current range to limit accumulated drift.
                bounds = lerp(aligned, bounds, 0.08);
            }
        }
        StableBounds = bounds;
        Range[0] = asuint(bounds.x);
        Range[1] = asuint(bounds.y);
        Range[2] = valid ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = index; valid && i < AnchorCount; i += 256) {
        uint2 p = SamplePosition(i);
        float raw = Prediction[p.y * Side + p.x];
        Range[AnchorBase + 2 * i + 1] = asuint(saturate((raw - StableBounds.x) / (StableBounds.y - StableBounds.x)));
    }
}

[numthreads(8, 8, 1)]
void Normalize(uint2 pixel : SV_DispatchThreadID) {
    float v = Prediction[pixel.y * Side + pixel.x];
    float lo = asfloat(Range[0]), hi = asfloat(Range[1]);
    Depth[pixel] = Range[2] && isfinite(v) ? 0.56 - 0.12 * saturate((v - lo) / (hi - lo)) : 0.5;
}
)";
