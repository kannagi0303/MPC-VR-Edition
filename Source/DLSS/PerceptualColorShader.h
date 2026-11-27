// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// P1: independent perceptual colour enhancement.
// The SAME pixel shader can process original SDR video (P1_STANDALONE),
// Normal A (P1_NORMAL_A), or RAW/Temporal B (P1_RAW_B). No extra drawing
// pass is needed when a LUT is already being rendered. Depth is optional.
// Colour-family tests are heuristics, NOT semantic object segmentation.
inline constexpr char PerceptualColorShader[] = R"(
Texture2D<float4> Picture : register(t0);
#if defined(P1_NORMAL_A) || defined(P1_RAW_B)
Texture2DArray<float4> Lut : register(t1);
SamplerState LutSampler : register(s1);
#endif
Texture2D<float> DepthColor : register(t2);
Texture2D<float4> FarDimmingMask : register(t3);
SamplerState DepthSampler : register(s2);
cbuffer Parameters : register(b0) {
    float4 Geometry;      // x,y: picture top-left; z,w: (width-1, height-1)
    float4 SourceControl; // x: Normal A grade blend; y,z: source offset; w: depth rotation
    float4 B21AndDepth;   // x: B2.1 luma participation; w: valid cached-depth confidence
    float4 Enhancements;  // x: Preferred Colors; y: Skin Tones; z: B2.1; w: adaptive B2.2
    float4 DepthTuning;   // x,y: reserved; z: max linear-light dim; w: LUT-only activation
    float4 OutputQuantization;
};
static const float3 YW = float3(.2126, .7152, .0722);
float Smooth(float a, float b, float x)
{
    float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}
float2 BinUV(float2 p) { return (p * 32.0 + .5) / 33.0; }
float Headroom(float source, float delta)
{
    if (abs(delta) < 1e-6) return 1.0;
    return (delta > 0.0 ? 1.0 - source : source) / abs(delta);
}
#if defined(P1_NORMAL_A)
float3 ApplyNormalA(float3 source, float3 grade, float fade)
{
    float3 delta = grade - source;
    float3 chroma = delta - dot(delta, YW);
    float pressure = (max(max(delta.r, delta.g), delta.b) -
        min(min(delta.r, delta.g), delta.b)) * (255.0 / 18.0);
    float limit = min(Headroom(source.r, chroma.r),
        min(Headroom(source.g, chroma.g), Headroom(source.b, chroma.b)));
    float base = min(limit, rsqrt(1.0 + pressure * pressure));
    float strength = fade <= 1.0 ? fade * base : min(limit, fade * base);
    return source + chroma * strength;
}
#endif

// Proactive enhancement, rather than a hue-drift protection clamp.
// Preserve incoming brightness (B2.1's result if enabled); adapt chroma
// in the source's existing hue family without forcing any universal skin tone.
// [VE-05 | 2026-11-24 16:39:20 +08:00] Preferred colours (sky/foliage).
// [VE-06 | 2026-11-24 16:39:20 +08:00] Skin tones, jointly weighted with preferred colours.
float3 Enhance(float3 source, float3 grade, float depthWeight)
{
    if (Enhancements.x <= 0.0 && Enhancements.y <= 0.0) return grade;
    float sy = dot(source, YW);
    float3 sc = source - sy;
    float cs = length(sc);
    if (sy <= .10 || sy >= .96 || cs <= .06 || cs >= .48) return grade;
    float3 axis = sc / max(cs, 1e-5);
    const float3 skinAxis = float3(.8153, -.1871, -.5479);
    const float3 foliageAxis = float3(-.6971, .2741, -.6625);
    const float3 skyAxis = float3(-.5990, .0978, .7947);
    float mid = Smooth(.11, .27, sy) * (1.0 - Smooth(.83, .96, sy));
    float moderate = Smooth(.055, .13, cs) * (1.0 - Smooth(.34, .48, cs));
    float weight = mid * moderate;
    // [VE-06] Source-family skin weight; no semantic segmentation.
    float skin = Enhancements.y * Smooth(.87, .985, dot(axis, skinAxis));
    // [VE-05] Source-family foliage/sky weights.
    float plant = Enhancements.x * Smooth(.86, .985, dot(axis, foliageAxis));
    float sky = Enhancements.x * Smooth(.87, .985, dot(axis, skyAxis));
    float selected = skin + plant + sky;
    if (selected < .002) return grade;

    float y = dot(grade, YW);
    float3 c = grade - y;
    float gc = length(c);
    if (gc <= .04) return grade;
    // Unrelated theatrical colour changes are not reinterpreted as daylight.
    float trust = Smooth(.45, .87, dot(c, axis) / max(gc, 1e-5));
    float w = saturate(selected * weight * trust * depthWeight);
    if (w <= .001) return grade;
    float3 preferred = skin * skinAxis + plant * foliageAxis + sky * skyAxis;
    preferred -= dot(preferred, YW);
    preferred *= rsqrt(max(dot(preferred, preferred), .0001));
    float hueBias = .045 * w;
    float3 adjusted = lerp(c, preferred * gc, hueBias);
    // Mildly boost selected chroma even when the source and grade already agree.
    // Tuned deliberately to be testable; the final release must be measured.
    float gain = 1.0 + .15 * w;
    adjusted *= gain;
    adjusted -= dot(adjusted, YW);
    // Reduce chroma only where it would exceed the output RGB gamut.
    float3 positive = max(adjusted, 0.0);
    float3 negative = max(-adjusted, 0.0);
    float hi = 1.0 - y;
    float lo = y;
    float fit = min(1.0, min(
        min(hi / max(positive.r, 1e-6), min(hi / max(positive.g, 1e-6), hi / max(positive.b, 1e-6))),
        min(lo / max(negative.r, 1e-6), min(lo / max(negative.g, 1e-6), lo / max(negative.b, 1e-6)))));
    return saturate(y + adjusted * saturate(fit));
}

float4 SvdLutGradeMain(float4 position)
{
    int2 p = int2(position.xy);
    int2 local = p - int2(Geometry.xy);
    float4 src = Picture.Load(int3(p + int2(SourceControl.yz), 0));
    if (any(local < 0) || any(local > int2(Geometry.zw))) return src;
    float3 original = saturate(src.rgb);
    float3 grade = original;
#if defined(P1_NORMAL_A) || defined(P1_RAW_B)
    uint edge = uint(local.x == int(Geometry.z)) + 2 * uint(local.y == int(Geometry.w));
    float layer = float(edge * 3);
    float3 rg = Lut.SampleLevel(LutSampler, float3(BinUV(original.rg), layer), 0).rgb;
    float3 rb = Lut.SampleLevel(LutSampler, float3(BinUV(original.rb), layer + 1), 0).rgb;
    float3 gb = Lut.SampleLevel(LutSampler, float3(BinUV(original.gb), layer + 2), 0).rgb;
#if defined(P1_NORMAL_A)
    grade = saturate(ApplyNormalA(original, rg + rb + gb, SourceControl.x));
#else
    grade = saturate(rg + rb + gb);
    if (Enhancements.z > .5) {
        grade = ApplyB21Luma(original, grade, B21AndDepth.x);
        if (Enhancements.w > .5)
            grade = ApplyB22Chroma(original, grade);
    }
    // B grade is mixed before perceptual enhancements: the latter stay independent.
    float b = saturate(SourceControl.x);
    grade = b >= 1.0 ? grade : lerp(original, grade, b);
#endif
#endif
    // [FA-01 | 2026-11-25 14:07 +08:00] Blend only the calibrated LUT contribution.
    // At zero activation, this equals the standalone path at the same AT/depth.
    // At full activation the previously accepted composition is unchanged.
#if defined(P1_RAW_B)
    float activation = saturate(DepthTuning.w);
    grade = activation >= 1.0 ? grade : lerp(original, grade, activation);
#endif
    if (Enhancements.x <= 0.0 && Enhancements.y <= 0.0 && DepthTuning.z <= 0.0) return float4(ApplyAdaptiveTone(grade,OutputQuantization.y), src.a);
    float depthWeight = 1.0;
    float farWeight = 0.0;
    if (B21AndDepth.w > .001) {
        float2 uv = (float2(local) + .5) / (Geometry.zw + 1.0);
        float2 sampleUV = SourceControl.w > .5 ? float2(1.0 - uv.y, uv.x) : uv;
        const float depthValue = DepthColor.SampleLevel(DepthSampler, sampleUV, 0);
        // Spatial mask smoothing stays; never blend depth from another PTS.
        depthWeight = 1.0 + B21AndDepth.w * .12 *
            clamp((depthValue - .5) / .06, -1.0, 1.0);
        // Depth Pro 2 RAW uses .44 (near), .50 (neutral), .56 (far).
        // Its normalized depth gain is independent of preferred/skin weights.
        if (DepthTuning.z>0.0) {
            farWeight=saturate(FarDimmingMask.SampleLevel(DepthSampler,sampleUV,0).x);
        }
    }
    // [AT-05 | 2026-11-25 12:03 +08:00] Keep LUT input and B2 references original;
    // tone-map the calibrated result before the joint preferred/skin colour stage.
    // Family detection continues to use the original (not the altered luma).
    float3 toned = ApplyAdaptiveTone(grade, OutputQuantization.y);
    float3 enhanced = Enhance(original, toned, depthWeight);
    // SDR RGB_FULL_G22_NONE_P709: uniform exposure gain in linear light,
    // mapped back to the gamma-2.2 signal. Soft highlight protection retains
    // bright sky/specular detail without disabling the far-depth effect.
    [branch] if (DepthTuning.z>0.0 && farWeight>0.0) {
        float y=dot(enhanced,YW);
        float protection=1.0-.65*Smooth(.80,.98,y);
        float loss=clamp(DepthTuning.z,0.0,.95)*farWeight*protection;
        enhanced*=pow(1.0-loss,1.0/2.2);
    }
    return float4(enhanced, src.a);
}
)";

// [VE01-FIX04] Optional dim-only low-resolution GPU passes. The history
// stores (mask, calibrated depth, raw luma, raw red-blue chroma). It is
// never used as a RAW depth input by NGX or the depth preview.
inline constexpr char FarDimmingShader[] = R"(
Texture2D<float4> RawPicture : register(t0);
Texture2D<float> RawDepth : register(t1);
Texture2D<float4> History : register(t2);
Texture2D<float4> Alignment : register(t3);
SamplerState Linear : register(s2);
cbuffer FarSettings : register(b0) {
    float4 Shape; // depth width/height, rotated, has compatible history
    float4 Timing; // static history retention, preceding-map fallback, exact history match, reserved
};
float SmoothFar(float a, float b, float x) {
    float t=saturate((x-a)/(b-a)); return t*t*(3.0-2.0*t);
}
float2 Features(float2 uv) {
    float2 stepUV=.25/Shape.xy;
    float3 rgb=0.0;
    [unroll] for(int y=-1;y<=1;y+=2) {
        [unroll] for(int x=-1;x<=1;x+=2) {
            float2 p=saturate(uv+float2(x,y)*stepUV);
            p=Shape.z>.5 ? float2(p.y,1.0-p.x) : p;
            rgb+=saturate(RawPicture.SampleLevel(Linear,p,0).rgb)*.25;
        }
    }
    return float2(dot(rgb,float3(.2126,.7152,.0722)),rgb.r-rgb.b);
}
float Stable(float2 a,float2 b) {
    float difference=max(abs(a.x-b.x),.6*abs(a.y-b.y));
    return 1.0-SmoothFar(.012,.09,difference);
}
#if defined(FAR_ALIGNMENT)
// Robust, bounded per-frame affine adjustment to the previous calibrated
// depth. Only apparently static samples vote; no reduction/readback on CPU.
float4 main(float4 pos:SV_POSITION):SV_TARGET {
    float n=0.0,sx=0.0,sy=0.0,sxx=0.0,sxy=0.0,stableCount=0.0;
    [loop] for(int y=0;y<8;++y) {
        [loop] for(int x=0;x<8;++x) {
            float2 uv=(float2(x,y)+.5)/8.0;
            float4 h=History.SampleLevel(Linear,uv,0);
            float a=RawDepth.SampleLevel(Linear,uv,0)-.5;
            float b=h.y-.5;
            float stable=Stable(Features(uv),h.zw);
            stableCount+=stable;
            float w=stable*(1.0-SmoothFar(.018,.045,abs(a-b)));
            n+=w; sx+=w*a; sy+=w*b; sxx+=w*a*a; sxy+=w*a*b;
        }
    }
    float trust=SmoothFar(22.0,43.0,stableCount);
    if(n<18.0 || trust<=0.0) return float4(1.0,0.0,0.0,0.0);
    float meanX=sx/n,meanY=sy/n;
    float variance=max(0.0,sxx/n-meanX*meanX);
    // Flat/neutral maps cannot acquire an artificial global depth offset.
    if(variance<.000045) return float4(1.0,0.0,trust,0.0);
    float scale=clamp((sxy/n-meanX*meanY)/variance,.85,1.15);
    float bias=clamp(meanY-scale*meanX,-.012,.012);
    float fitTrust=trust*SmoothFar(18.0,38.0,n);
    return float4(lerp(1.0,scale,fitTrust),bias*fitTrust,trust,0.0);
}
#else
float Calibrated(float d,float2 fit) {
    return clamp(.5+(d-.5)*fit.x+fit.y,.44,.56);
}
float4 main(float4 pos:SV_POSITION):SV_TARGET {
    float2 uv=pos.xy/Shape.xy;
    float2 features=Features(uv);
    float4 fit=float4(1.0,0.0,0.0,0.0);
    [branch] if(Shape.w>.5 && Timing.y<.5) fit=Alignment.Load(int3(0,0,0));
    float center=Calibrated(RawDepth.SampleLevel(Linear,uv,0),fit.xy);
    float2 texel=4.0/Shape.xy;
    float sum=0.0, depthSum=0.0;
    [unroll] for(int y=-1;y<=1;++y) {
        [unroll] for(int x=-1;x<=1;++x) {
            float w=(x==0?2.0:1.0)*(y==0?2.0:1.0);
            float d=Calibrated(RawDepth.SampleLevel(Linear,uv+float2(x,y)*texel,0),fit.xy);
            sum+=w*SmoothFar(.50,.56,d); depthSum+=w*d;
        }
    }
    float candidate=sum/16.0;
    float softenedDepth=depthSum/16.0;
    float result=candidate;
    [branch] if(Shape.w>.5) {
        float4 h=History.SampleLevel(Linear,uv,0);
        float stable=Stable(features,h.zw);
        if(Timing.y>.5) {
            // A genuinely late original may borrow its immediately preceding
            // mask. Do not carry that matte across a changed raw image region.
            if(Timing.z>.5) result=h.x*stable;
        } else {
            float delta=abs(candidate-h.x);
            float agreement=1.0-SmoothFar(.15,.55,delta);
            float keep=Timing.x*fit.z*stable*agreement;
            // Tiny changes in an otherwise static region should not flicker.
            if(delta<.007 && keep>.85) result=h.x;
            else result=lerp(candidate,h.x,keep);
            // No long-lived history in newly near/neutral regions; retain
            // the spatial penumbra but release a disappearing far matte.
            if(softenedDepth<=.50)
                result=min(result,candidate+(1.0-candidate)*SmoothFar(.495,.50,softenedDepth)*.035);
        }
    }
    return float4(saturate(result),center,features);
}
#endif
)";
