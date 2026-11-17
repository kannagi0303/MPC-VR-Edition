// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace mfg {
inline constexpr char MotionShaders[]=R"(
Texture2D<float4> Frame : register(t0);
Texture2D<int2> Flow : register(t1);
cbuffer Params : register(b0) {uint2 size;uint2 inputSize;uint grid;uint detectBars;uint2 unused;int4 picture;};
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
Texture2D<float2> Rows : register(t4);
Texture2D<int4> PreviousPicture : register(t5);
Texture2D<int4> Active : register(t6);

float2 PictureRows(float4 p:SV_POSITION):SV_TARGET {
    const int y = int(p.y);
    if (y < picture.y || y >= picture.w) return 0;
    float lit = 0, sum = 0;
    [loop] for (int i = 0; i < 48; ++i) {
        const int x = picture.x + (2 * i + 1) * (picture.z - picture.x) / 96;
        const float3 rgb = Frame.Load(int3(x, y, 0)).rgb;
        const float level = max(rgb.r, max(rgb.g, rgb.b));
        lit += level > 0.035 ? 1.0 : 0.0;
        sum += level;
    }
    return float2(lit, sum / 48.0);
}

int DarkRows(int first, int step, int limit, float litLimit, float meanLimit) {
    [loop] for (int i = 0; i < limit; ++i) {
        const float2 row = Rows.Load(int3(0, first + step * i, 0));
        if (row.x >= litLimit || row.y >= meanLimit) return i;
    }
    return limit;
}

bool VisibleEdge(int first, int step) {
    [loop] for (int i = 0; i < 8; ++i)
        if (Rows.Load(int3(0, first + step * i, 0)).y >= 0.085) return true;
    return false;
}

bool HasInteriorContrast(int top, int bottom) {
    [unroll] for (int i = 0; i < 12; ++i) {
        const int y = top + (2 * i + 1) * (bottom - top) / 24;
        const float2 row = Rows.Load(int3(0, y, 0));
        if (row.x >= 8 && row.y >= 0.035) return true;
    }
    return false;
}

struct PictureOutput {
    int4 active : SV_TARGET0;
    int4 history : SV_TARGET1;
};

PictureOutput ActivePicture() {
    PictureOutput result;
    result.active = picture;
    result.history = 0;
    const int height = picture.w - picture.y;
    if (!(detectBars & 1) || height < 64) return result;
    const int4 prior = PreviousPicture.Load(int3(0, 0, 0));
    if (prior.z >= 3) {
        // Check the existing bands for new picture content before trusting
        // dim edges or a different boundary estimate from this frame.
        const int top = DarkRows(picture.y, 1, prior.x, 8.0, 0.055);
        const int bottom = DarkRows(picture.w - 1, -1, prior.y, 8.0, 0.055);
        if (top == prior.x && bottom == prior.y) {
            const bool visible = HasInteriorContrast(picture.y + top, picture.w - bottom);
            const int ambiguous = visible ? 0 : prior.w + 1;
            // A short fade can hide both edges. Give up after eight frames
            // with no distinguishable picture, and require fresh evidence.
            if (ambiguous <= 8) {
                result.history = int4(top, bottom, 3, ambiguous);
                if (detectBars & 2) {
                    result.active.y += top;
                    result.active.w -= bottom;
                }
                return result;
            }
        }
    }

    const int limit = height / 4 + 1;
    const int top = DarkRows(picture.y, 1, limit, 3.0, 0.04);
    const int bottom = DarkRows(picture.w - 1, -1, limit, 3.0, 0.04);
    const int minimum = max(8, height / 32);
    if (top < minimum || bottom < minimum || top == limit || bottom == limit ||
        abs(top - bottom) > max(4, height / 80))
        return result;
    if (!VisibleEdge(picture.y + top, 1) || !VisibleEdge(picture.w - 1 - bottom, -1))
        return result;

    const bool stable = prior.z > 0 && prior.z < 3 && abs(top - prior.x) <= 2 && abs(bottom - prior.y) <= 2;
    // Keep the confirmed boundary conservative when the estimate drifts a pixel.
    const int count = stable ? min(prior.z + 1, 3) : 1;
    result.history = int4(stable ? min(top, prior.x) : top, stable ? min(bottom, prior.y) : bottom, count, 0);
    if (count >= 3 && (detectBars & 2)) {
        result.active.y += result.history.x;
        result.active.w -= result.history.y;
    }
    return result;
}

float2 MfgVectors(float4 p:SV_POSITION):SV_TARGET {
    const int4 bounds = Active.Load(int3(0, 0, 0));
    const int2 pixel = int2(p.xy);
    if (any(pixel < bounds.xy) || any(pixel >= bounds.zw)) return 0;
    return Vectors(p);
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
