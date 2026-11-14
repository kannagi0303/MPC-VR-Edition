// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SR-only, edge-aware expansion of NVOFA's 4x4 block flow into a dense
// input-resolution motion texture.  The MFG flow/shaders are NOT changed.
// The center cell remains authoritative at motion discontinuities: only
// neighboring cells with consistent motion participate in interpolation.
namespace sr_motion {
inline constexpr char RefineShaders[] = R"(
Texture2D<int2> Flow : register(t1);
Texture2D<int2> ReverseFlow : register(t2);
Texture2D<uint> ForwardCost : register(t3);
cbuffer Params : register(b0) { uint2 size; uint2 inputSize; uint grid; uint pad; uint2 unused; };

float2 ReadFlow(int2 cell, int2 maxCell) {
    return float2(Flow.Load(int3(clamp(cell, int2(0,0), maxCell), 0))) / 32.0;
}

float2 DenseFlow(uint2 xy) {
    const int2 maxCell = int2((inputSize + grid - 1u) / grid) - int2(1,1);
    const float2 center = ReadFlow(int2(xy / grid), maxCell);
    // Grid vectors represent block centers, not their upper-left corners.
    const float2 gridPos = (float2(xy) + 0.5) / float(grid) - 0.5;
    const int2 origin = int2(floor(gridPos));
    const float2 blend = frac(gridPos);
    const float2 f00 = ReadFlow(origin, maxCell);
    const float2 f10 = ReadFlow(origin + int2(1,0), maxCell);
    const float2 f01 = ReadFlow(origin + int2(0,1), maxCell);
    const float2 f11 = ReadFlow(origin + int2(1,1), maxCell);
    // Reject blending across motion boundaries/occlusions. This is a
    // spatial interpolation, NOT a temporal-history confidence mask.
    const float tolerance = 1.25 + 0.05 * length(center);
    const float4 baseWeights = float4((1-blend.x)*(1-blend.y),
        blend.x*(1-blend.y), (1-blend.x)*blend.y, blend.x*blend.y);
    const float4 weights = baseWeights * float4(
        length(f00-center)<=tolerance, length(f10-center)<=tolerance,
        length(f01-center)<=tolerance, length(f11-center)<=tolerance);
    const float total = dot(weights, float4(1,1,1,1));
    return total > 0.0001 ?
        (f00*weights.x + f10*weights.y + f01*weights.z + f11*weights.w) / total
        : center;
}

float2 Validate(uint2 xy, bool useCost) {
    const float2 forward = DenseFlow(xy);
    const float2 previous = float2(xy) + 0.5 + forward;
    if (any(previous < 0.0) || any(previous >= float2(inputSize)))
        return float2(0,0);
    const float2 backward = float2(ReverseFlow.Load(int3(uint2(previous)/grid,0))) / 32.0;
    const float residual = length(forward + backward);
    float tolerance = 1.5 + 0.05 * length(forward);
    if (useCost) {
        const uint cost = ForwardCost.Load(int3(xy/grid,0));
        const float expensive = smoothstep(0.5, 1.0, float(cost)/255.0);
        tolerance *= 1.0 - 0.2 * expensive;
    }
    // Match the existing validated SR behavior for genuinely unreliable
    // vectors; do not fabricate depth or manipulate the NGX temporal mask.
    return residual > tolerance ? float2(0,0) : forward;
}
float2 VectorsValidatedSR(float4 p:SV_POSITION):SV_TARGET {
    return Validate(uint2(p.xy), false);
}
float2 VectorsValidatedCostSR(float4 p:SV_POSITION):SV_TARGET {
    return Validate(uint2(p.xy), true);
}
)";
}
