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
