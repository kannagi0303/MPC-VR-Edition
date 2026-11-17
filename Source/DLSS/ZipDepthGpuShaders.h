// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr char ZipDepthGpuShaders[] = R"(
Texture2D<float4> Capture : register(t0);
RWStructuredBuffer<float> Image : register(u0);
RWStructuredBuffer<float> Prediction : register(u1);
RWTexture2D<float> Depth : register(u2);
RWStructuredBuffer<uint> Range : register(u3);
cbuffer Settings : register(b0) {
    uint HasPrevious;
    uint ModelWidth;
    uint ModelHeight;
    uint ElapsedPts;
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

groupshared float Samples[1024];
groupshared float2 SceneMapping;
groupshared uint UpdateAnchors;
groupshared float4 AnchorSums[256]; // current, previous, current², previous²
groupshared float2 AnchorCross[256]; // current * previous, count
groupshared uint4 SceneSums[256]; // large difference, brightened, darkened, chroma change
// Bounds and anchors use scene coordinates. Mapping converts raw model
// predictions into that space so a successful fit does not move the bounds.
static const uint AnchorBase = 6;
static const uint AnchorCount = 1024;
static const uint LastRgbBase = AnchorBase + 2 * AnchorCount;

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

float MoveBound(float before, float target, float span, float seconds, bool expands, bool reliable) {
    const float time = expands ? 0.12 : 0.65;
    const float rate = reliable ? 1.0 : 0.30;
    const float limit = span * min(0.12, (expands ? 2.0 : 0.6) * seconds) * rate;
    const float amount = (target - before) * (1.0 - exp(-seconds * rate / time));
    return before + clamp(amount, -limit, limit);
}

// Sort a 32x32 grid from the model input.
// Run this once per estimate, not for each pixel of the depth texture.
[numthreads(256, 1, 1)]
void Percentiles(uint index : SV_GroupIndex) {
    for (uint sampleIndex = index; sampleIndex < AnchorCount; sampleIndex += 256) {
        uint2 p = SamplePosition(sampleIndex);
        float v = Prediction[p.y * ModelWidth + p.x];
        Samples[sampleIndex] = FiniteDepth(v) ? v : asfloat(0x7f800000u);
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
    const bool valid = FiniteDepth(Samples[1023]) && hi > lo + 1e-5;
    const float2 oldBounds = HasPrevious ? float2(asfloat(Range[0]), asfloat(Range[1])) : float2(0, 1);
    const float oldSpan = max(oldBounds.y - oldBounds.x, 1e-5);
    // Compare sparse points at the same location when their source RGB still
    // matches. Fit only a global affine mapping so moving objects can change depth.
    float4 sums = 0.0;
    float2 cross = 0.0;
    uint4 scene = 0;
    for (uint i = index; valid && i < AnchorCount; i += 256) {
        uint2 p = SamplePosition(i);
        float3 rgb = Capture.Load(int3(p, 0)).rgb;
        float raw = Prediction[p.y * ModelWidth + p.x];
        if (HasPrevious) {
            const float3 change = rgb - UnpackRgb(Range[LastRgbBase + i]);
            const float brightness = dot(change, float3(0.2126, 0.7152, 0.0722));
            const float3 chroma = abs(change - brightness);
            const float3 absoluteChange = abs(change);
            scene += uint4(max(absoluteChange.x, max(absoluteChange.y, absoluteChange.z)) > 0.20,
                brightness > 0.14, brightness < -0.14, max(chroma.x, max(chroma.y, chroma.z)) > 0.15);
            float3 delta = abs(rgb - UnpackRgb(Range[AnchorBase + 2 * i]));
            float prior = (asfloat(Range[AnchorBase + 2 * i + 1]) - oldBounds.x) / oldSpan;
            // Exclude black bars and very dark areas as scene anchors.
            if (max(max(rgb.r, rgb.g), rgb.b) > 0.06 &&
                max(max(delta.r, delta.g), delta.b) < 0.04) {
                const float x = (raw - lo) / (hi - lo);
                if (x < -0.25 || x > 1.25 || prior < -0.5 || prior > 1.5) continue;
                sums += float4(x, prior, x * x, prior * prior);
                cross += float2(x * prior, 1.0);
            }
        }
        Range[LastRgbBase + i] = PackRgb(rgb);
    }
    AnchorSums[index] = sums;
    AnchorCross[index] = cross;
    SceneSums[index] = scene;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) {
            AnchorSums[index] += AnchorSums[index + stride];
            AnchorCross[index] += AnchorCross[index + stride];
            SceneSums[index] += SceneSums[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        const uint4 scene = SceneSums[0];
        // Sparse fallback for manual MFG. A fade mostly changes luminance in
        // one direction, while a large cut changes spatial or chroma content.
        const bool cut = HasPrevious && valid && scene.x >= 740 &&
            ((scene.y >= 210 && scene.z >= 210) || scene.w >= 420);
        const bool continuous = HasPrevious && !cut;
        float2 bounds = continuous ? oldBounds : float2(0, 1);
        float2 mapping = continuous ? float2(asfloat(Range[3]), asfloat(Range[4])) :
            float2(1.0, -lo) / max(hi - lo, 1e-5);
        const float seconds = float(ElapsedPts) * 1e-7;
        const float count = AnchorCross[0].y;
        bool reliable = false;
        if (continuous && valid && count >= 192.0) {
            float4 mean = AnchorSums[0] / count;
            float cov = AnchorCross[0].x / count - mean.x * mean.y;
            float varX = max(0.0, mean.z - mean.x * mean.x);
            float varY = max(0.0, mean.w - mean.y * mean.y);
            if (min(varX, varY) >= 0.005 && cov > 0.0 && cov * cov >= 0.7 * varX * varY) {
                const float gain = cov / varX;
                const float offset = mean.y - gain * mean.x;
                if (gain >= 0.33 && gain <= 3.0 && abs(offset) <= 1.5) {
                    mapping.x = oldSpan * gain / (hi - lo);
                    mapping.y = oldBounds.x + oldSpan * offset - mapping.x * lo;
                    reliable = true;
                }
            }
        }
        const float anchorAge = continuous ? asfloat(Range[5]) + seconds : 0.0;
        if (continuous && valid) {
            const float2 target = float2(lo, hi) * mapping.x + mapping.y;
            bounds.x = MoveBound(oldBounds.x, target.x, oldSpan, seconds, target.x < oldBounds.x, reliable);
            bounds.y = MoveBound(oldBounds.y, target.y, oldSpan, seconds, target.y > oldBounds.y, reliable);
            const float span = bounds.y - bounds.x;
            if (span < 0.25)
                bounds = lerp(oldBounds, bounds, saturate((oldSpan - 0.25) / max(oldSpan - span, 1e-5)));
        }
        // A short run of bad fits retains the last trusted anchors. If the
        // camera moves on, eventually take anchors in the same scene space.
        UpdateAnchors = valid && (!continuous || reliable || (anchorAge >= 0.45 && count < 192.0));
        SceneMapping = mapping;
        Range[2] = uint(valid) | (uint(cut) << 1);
        if (valid) {
            // FXC /O3 can eliminate the first plain UAV store here.
            uint oldBound;
            InterlockedExchange(Range[0], asuint(bounds.x), oldBound);
            InterlockedExchange(Range[1], asuint(bounds.y), oldBound);
            Range[3] = asuint(mapping.x);
            Range[4] = asuint(mapping.y);
            Range[5] = asuint(UpdateAnchors ? 0.0 : anchorAge);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint anchorIndex = index; UpdateAnchors && anchorIndex < AnchorCount; anchorIndex += 256) {
        uint2 p = SamplePosition(anchorIndex);
        float raw = Prediction[p.y * ModelWidth + p.x];
        Range[AnchorBase + 2 * anchorIndex] = PackRgb(Capture.Load(int3(p, 0)).rgb);
        Range[AnchorBase + 2 * anchorIndex + 1] = asuint(raw * SceneMapping.x + SceneMapping.y);
    }
}

[numthreads(8, 8, 1)]
void Normalize(uint2 pixel : SV_DispatchThreadID) {
    float v = Prediction[pixel.y * ModelWidth + pixel.x];
    float lo = asfloat(Range[0]), hi = asfloat(Range[1]);
    const float aligned = v * asfloat(Range[3]) + asfloat(Range[4]);
    Depth[pixel] = (Range[2] & 1u) && FiniteDepth(aligned)
        ? 0.56 - 0.12 * saturate((aligned - lo) / max(hi - lo, 1e-5)) : 0.5;
}
)";
