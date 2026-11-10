// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace mfg {
inline constexpr char MotionShaders[]=R"(
Texture2D<float4> Frame : register(t0);
Texture2D<int2> Flow : register(t1);
cbuffer Params : register(b0) {uint2 size;uint2 inputSize;uint grid;uint pad;uint2 unused;};
float4 VS(uint id:SV_VertexID):SV_POSITION {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float Luma(float4 p:SV_POSITION):SV_TARGET {
    return dot(Frame.Load(int3(uint2(p.xy),0)).rgb,float3(.2126,.7152,.0722));
}
float4 Copy(float4 p:SV_POSITION):SV_TARGET {
    float2 at=p.xy*float2(inputSize)/float2(size)-0.5;
    int2 origin=int2(floor(at));float2 t=frac(at);
    int2 maximum=int2(inputSize)-1;
    int2 p00=clamp(origin,int2(0,0),maximum);
    int2 p10=clamp(origin+int2(1,0),int2(0,0),maximum);
    int2 p01=clamp(origin+int2(0,1),int2(0,0),maximum);
    int2 p11=clamp(origin+int2(1,1),int2(0,0),maximum);
    return lerp(lerp(Frame.Load(int3(p00,0)),Frame.Load(int3(p10,0)),t.x),
                lerp(Frame.Load(int3(p01,0)),Frame.Load(int3(p11,0)),t.x),t.y);
}
float2 Vectors(float4 p:SV_POSITION):SV_TARGET {
    return float2(Flow.Load(int3(uint2(p.xy)/grid,0)))/32.0;
}
)";
}
