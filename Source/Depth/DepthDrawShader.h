// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr char DepthDrawShader[] = R"(
Texture2D<float> NewDepth : register(t0);
Texture2D<int4> ActivePicture : register(t5);
SamplerState Linear : register(s0);

cbuffer Settings : register(b0) {
    float4 Picture; // origin and extent in consumer pixels
    float4 DepthSettings; // rotation, reverse Z, active bounds, output depth span
};

float2 ModelUV(float2 uv) {
    return DepthSettings.x > 0.5 ? float2(1.0 - uv.y, uv.x) : uv;
}

bool Inside(float2 p, float4 bounds) {
    return all(p >= bounds.xy) && all(p < bounds.zw);
}

float4 Bounds() {
    float4 bounds = float4(Picture.xy, Picture.xy + Picture.zw);
    if (DepthSettings.z > 0.5) {
        const float4 active = ActivePicture.Load(int3(0, 0, 0));
        bounds = float4(max(bounds.xy, active.xy), min(bounds.zw, active.zw));
    }
    return bounds;
}

float RawExpand(float4 pos : SV_POSITION) : SV_TARGET {
    if (!Inside(pos.xy, Bounds())) return 0.5;
    float depth = NewDepth.SampleLevel(Linear, ModelUV((pos.xy - Picture.xy) / Picture.zw), 0);
    if (DepthSettings.y > 0.5) depth = 1.0 - depth;
    return 0.5 + (depth - 0.5) * (DepthSettings.w / 0.12);
}
)";
