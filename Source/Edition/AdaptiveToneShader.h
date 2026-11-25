// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// [AT-03 | 2026-11-25 11:25 +08:00] Shared final code-luma/chroma-ray mapping.
inline constexpr char AdaptiveToneShader[] = R"(
cbuffer AdaptiveToneCurve : register(b1) { float4 ATCurve[65]; };
float ATValue(uint i) { return ATCurve[i >> 2][i & 3]; }
float3 ApplyAdaptiveTone(float3 rgb, float weight) {
    if (weight <= 0.0) return rgb;
    const float3 w=float3(.2126,.7152,.0722);
    float y=saturate(dot(rgb,w));
    float p=y*256.0;
    uint i=min((uint)p,255u);
    float z=lerp(y,lerp(ATValue(i),ATValue(i+1),p-i),saturate(weight));
    float3 c=rgb-y;
    float hi=max(max(c.r,c.g),max(c.b,1e-6));
    float lo=max(max(-c.r,-c.g),max(-c.b,1e-6));
    return saturate(z+c*saturate(min((1-z)/hi,z/lo)));
}
)";
inline constexpr char AdaptiveToneStandalone[] = R"(
Texture2D<float4> Picture : register(t0);
// [AT-06 | 2026-11-25 12:20 +08:00] Reuse the existing simple scaler
// when geometry is changing, rather than adding a full-size post-pass.
SamplerState PictureSampler : register(s0);
cbuffer Parameters : register(b0) { float4 Control; float4 Mask; float4 Sampling; };
Texture2D<float4> ATDither : register(t4);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    float4 src;
    if(Sampling.x>0.0) src=Picture.Sample(PictureSampler,uv);
    else src=Picture.Load(int3(int2(position.xy)+int2(Control.xy),0));
    if(any(position.xy<Mask.xy) || any(position.xy>=Mask.zw)) return src;
    float3 rgb=ApplyAdaptiveTone(src.rgb,Control.z);
    if(Control.w>0.0) {
        float3 code=saturate(rgb)*Control.w;
        float3 nearest=round(code);
        int2 p=int2(position.xy);
        float threshold=min(ATDither.Load(int3(p.x & 31,p.y & 31,0)).r,0.999999);
        float3 rounded=floor(code+threshold);
        rgb=float3(abs(code.r-nearest.r)<.0002?nearest.r:rounded.r,abs(code.g-nearest.g)<.0002?nearest.g:rounded.g,abs(code.b-nearest.b)<.0002?nearest.b:rounded.b)/Control.w;
    }
    return float4(rgb,src.a);
}
)";

// FP32 half-pixel bilinear, independent of the VE model / downscale sampler.
inline constexpr char AdaptiveToneThumbnail[] = R"(
Texture2D<float4> Picture : register(t0);
cbuffer InputRect : register(b0) { float4 Rect; };
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 p=clamp(position.xy*Rect.zw/float2(256.0,144.0)-0.5,0.0,Rect.zw-1.0);
    int2 lo=int2(floor(p)), hi=min(lo+1,int2(Rect.zw)-1), origin=int2(Rect.xy);
    float2 w=p-float2(lo);
    float3 a=Picture.Load(int3(origin+lo,0)).rgb;
    float3 b=Picture.Load(int3(origin+int2(hi.x,lo.y),0)).rgb;
    float3 c=Picture.Load(int3(origin+int2(lo.x,hi.y),0)).rgb;
    float3 d=Picture.Load(int3(origin+hi,0)).rgb;
    return float4(saturate(lerp(lerp(a,b,w.x),lerp(c,d,w.x),w.y)),1.0);
}
)";
