// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

inline constexpr char ZipDepthTemporalShaders[] = R"(
Texture2D<float> NewDepth : register(t0);
Texture2D<float4> History : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float> CurrentLuma : register(t3);
Texture2D<float> PreviousLuma : register(t4);
Texture2D<int4> ActivePicture : register(t5);
Texture2D<int4> PriorPicture : register(t6);
SamplerState Linear : register(s0);

cbuffer Settings : register(b0) {
    float4 Picture; // origin and extent in consumer pixels
    float4 Model;   // rotate, elapsed estimate time in seconds, fresh estimate, history
    float4 Frame;   // consumer size, estimate age in frames, reverse Z
    float4 Pair;    // compare luma, estimate from prior frame, active bounds, prior bounds
    float4 Grid;    // model dimensions in source orientation, DLSS output depth span
};

float2 ModelUV(float2 uv) {
    return Model.x > 0.5 ? float2(1.0 - uv.y, uv.x) : uv;
}

bool Inside(float2 p, float4 bounds) {
    return all(p >= bounds.xy) && all(p < bounds.zw);
}

float4 Bounds() {
    float4 bounds = float4(Picture.xy, Picture.xy + Picture.zw);
    if (Pair.z > 0.5) {
        const float4 active = ActivePicture.Load(int3(0, 0, 0));
        bounds = float4(max(bounds.xy, active.xy), min(bounds.zw, active.zw));
    }
    return bounds;
}

float2 CompareLuma(float2 p, float2 q, float4 bounds, bool canWarp) {
    float2 error = 0;
    const float2 first = bounds.xy + 0.5, last = bounds.zw - 0.5;
    const float2 step = clamp(Picture.zw / Grid.xy * 0.35, 1.0, 3.0);
    const int2 offsets[5] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    [unroll] for (int i = 0; i < 5; ++i) {
        const float2 delta = offsets[i] * step;
        const float2 at = clamp(p + delta, first, last) / Frame.xy;
        const float current = CurrentLuma.SampleLevel(Linear, at, 0);
        error.x += abs(current - PreviousLuma.SampleLevel(Linear, at, 0));
        if (canWarp) {
            const float2 prior = clamp(q + delta, first, last) / Frame.xy;
            error.y += abs(current - PreviousLuma.SampleLevel(Linear, prior, 0));
        }
    }
    return float2(error.x * 0.2, canWarp ? error.y * 0.2 : 1.0);
}

// History channels are depth, signed run of consistent estimate changes,
// and validity. A genuine 0.5 depth is still a valid history sample.
float4 Stabilize(float4 pos : SV_POSITION) : SV_TARGET {
    const float2 p = Picture.xy + pos.xy / Grid.xy * Picture.zw;
    const float4 bounds = Bounds();
    if (!Inside(p, bounds)) return float4(0.5, 0, 0, 0);

    const float2 motion = Motion.Load(int3(int2(p), 0));
    const float speed = length(motion);
    const float2 q = p + motion;
    const bool canWarp = Inside(q, bounds);
    const bool sameBounds = Pair.z < 0.5 || (Pair.w > 0.5 &&
        all(ActivePicture.Load(int3(0, 0, 0)) == PriorPicture.Load(int3(0, 0, 0))));
    float2 prior = q;
    float confidence = canWarp ? 0.35 : 0.0;
    float stationary = 0.0;
    if (Pair.x > 0.5 && sameBounds) {
        const float2 error = CompareLuma(p, q, bounds, canWarp);
        const bool still = error.x <= error.y + 0.006;
        prior = still ? p : q;
        confidence = 1.0 - smoothstep(0.012, 0.065, still ? error.x : error.y);
        stationary = confidence * (1.0 - smoothstep(0.5, 3.0, speed)) *
            (1.0 - smoothstep(0.006, 0.025, error.x));
    }
    if (!sameBounds || !Inside(prior, bounds)) confidence = 0.0;
    const float4 history = Model.w > 0.5 && confidence > 0.0
        ? History.SampleLevel(Linear, (prior - Picture.xy) / Picture.zw, 0) : float4(0.5, 0, 0, 0);
    const bool trusted = history.z >= 0.995 && confidence >= 0.12;
    if (Model.z < 0.5) return trusted ? history : float4(0.5, 0, 0, 0);

    float2 freshAt = p;
    if (Frame.z >= 0.5) {
        // A delayed estimate can be aligned only where the image still
        // matches the available source pair.
        if (confidence < 0.12) return trusted ? history : float4(0.5, 0, 0, 0);
        freshAt = Pair.y > 0.5 ? prior : p + (prior - p) * min(Frame.z, 4.0);
        if (!Inside(freshAt, bounds)) return trusted ? history : float4(0.5, 0, 0, 0);
    }
    float fresh = NewDepth.SampleLevel(Linear, ModelUV((freshAt - Picture.xy) / Picture.zw), 0);
    if (Frame.w > 0.5) fresh = 1.0 - fresh;
    if (!trusted) return float4(fresh, 0, 1, 0);

    const float delta = fresh - history.x;
    const float direction = sign(delta);
    const float trend = abs(delta) >= 0.002 && stationary > 0.55
        ? direction * (sign(history.y) == direction ? min(abs(history.y) + 1.0, 6.0) : 1.0) : 0.0;
    const float persist = smoothstep(2.0, 5.0, abs(trend));
    const float seconds = clamp(Model.y, 0.0083, 0.25);
    const float stableWeight = lerp(1.0 - exp(-seconds / 0.32), 0.55, persist);
    const float movingWeight = lerp(1.0, 0.48, confidence);
    const float weight = lerp(movingWeight, stableWeight, stationary);
    const float limit = lerp(0.12, lerp(0.085 * seconds, 0.024, persist), stationary);
    return float4(clamp(history.x + clamp(delta * weight, -limit, limit), 0.44, 0.56), trend, 1, 0);
}

float Expand(float4 pos : SV_POSITION) : SV_TARGET {
    if (!Inside(pos.xy, Bounds())) return 0.5;
    // Invalid grid cells have neutral depth. Bilinear expansion avoids a
    // block boundary when a pixel moves into valid history.
    const float depth = History.SampleLevel(Linear, (pos.xy - Picture.xy) / Picture.zw, 0).x;
    // Keep the stabilized history in its existing range. Expand only the
    // submitted texture, with the same neutral point for SR and reverse-Z MFG.
    const float relative = clamp((depth - 0.5) / 0.12, -0.5, 0.5);
    return 0.5 + relative * Grid.z;
}
)";
