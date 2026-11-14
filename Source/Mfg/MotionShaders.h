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
// Optional video temporal-validation pass: the reverse vector should bring
// the previous position back to the current frame. Reject inconsistent flow
// without emitting the legacy DLSS bias-current-color mask (unsupported by
// the current NGX J/K/L/M presets).
Texture2D<int2> ReverseFlow : register(t2);
float2 VectorsValidated(float4 p:SV_POSITION):SV_TARGET {
    const uint2 xy=uint2(p.xy);
    const float2 forward=float2(Flow.Load(int3(xy/grid,0)))/32.0;
    const float2 prior=float2(xy)+float2(0.5,0.5)+forward;
    const bool outside=any(prior<0.0)||any(prior>=float2(inputSize));
    bool inconsistent=outside;
    if(!outside) {
        const float2 backward=float2(ReverseFlow.Load(int3(uint2(prior)/grid,0)))/32.0;
        // NVOFA vectors are sampled per grid cell. Allow quantization and
        // faster motion some tolerance before rejecting temporal reuse.
        const float residual=length(forward+backward);
        inconsistent=residual>(1.5+0.05*length(forward));
    }
    return inconsistent?float2(0.0,0.0):forward;
}
// Optional SR-only path: blend hardware matching cost with the existing
// forward/backward consistency test. A high cost only tightens the residual
// threshold; it never invalidates an otherwise consistent vector by itself.
// The numerical cost is a heuristic, NOT a calibrated occlusion probability.
Texture2D<uint> ForwardCost : register(t3);
float2 VectorsValidatedCost(float4 p:SV_POSITION):SV_TARGET {
    const uint2 xy=uint2(p.xy);
    const float2 forward=float2(Flow.Load(int3(xy/grid,0)))/32.0;
    const float2 prior=float2(xy)+float2(0.5,0.5)+forward;
    if (any(prior<0.0)||any(prior>=float2(inputSize)))
        return float2(0.0,0.0);
    const float2 backward=float2(ReverseFlow.Load(int3(uint2(prior)/grid,0)))/32.0;
    const float residual=length(forward+backward);
    const uint cost=ForwardCost.Load(int3(xy/grid,0));
    // Only the highest-cost cells have a meaningfully tighter threshold.
    const float highCost=smoothstep(0.5, 1.0, float(cost)/255.0);
    const float tolerance=(1.5+0.05*length(forward))*(1.0-0.2*highCost);
    return residual>tolerance?float2(0.0,0.0):forward;
}
)";
}
