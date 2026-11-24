// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// B2.1: independent code-luma participation, not multiplicative exposure.
// W3C SetLum/ClipColor principle with Rec.709 coefficients (not W3C's RGB weights).
// Keep the model and B1 LUT immutable. CPU and GPU use the same math.
#include "SvdLutB2Refined.h"
namespace svdlut::b2::luma {
using RGB = std::array<float, 3>;
inline constexpr float MaximumParticipation = .25f;
inline constexpr float MaxCodeShift = 4.0f / 255.0f;
inline constexpr RGB NoLuma{0,0,0};

inline RGB Apply(const RGB& source, const RGB& grade, float participation)
{
    if (participation >= 1.0f) return grade; // B2.1 off: exact B1
    const float sy = CodeLuma(source), gy = CodeLuma(grade);
    const float y = Unit(sy + (std::clamp)(Unit(participation) * (gy - sy), -MaxCodeShift, MaxCodeShift));
    const RGB c{grade[0]-gy, grade[1]-gy, grade[2]-gy};
    const float hi = (std::max)({c[0],c[1],c[2],1e-6f});
    const float lo = (std::max)({-c[0],-c[1],-c[2],1e-6f});
    const float fit = Unit((std::min)((1-y)/hi,y/lo));
    return {Unit(y+c[0]*fit), Unit(y+c[1]*fit), Unit(y+c[2]*fit)};
}

// A single weight avoids introducing new zone boundaries or changing hue
// as an incidental consequence of exposure correction. The per-pixel cap
// also protects colours missed by the sparse source samples. The neutral
// response probes are a safety envelope, not a scene classifier or evidence
// of actually observed pixels. All four packed edge variants are checked.
inline Result Analyze(std::span<const float> samples, std::span<const float> lut)
{
    Result r{}; r.tone.gains = NoLuma;
    if (samples.size() != refined::SampleValues || lut.size() != Values) return r;
    float cap = MaximumParticipation;
    for (unsigned i=0; i<refined::SampleCount; ++i) {
        RGB s{samples[3*i],samples[3*i+1],samples[3*i+2]};
        for (float& v:s) { if (!std::isfinite(v)) return r; v=Unit(v); }
        const auto g=ApplyB1(lut,s);
        const float d=std::abs(CodeLuma(g)-CodeLuma(s));
        if (!std::isfinite(d)) return r;
        if (d>0) cap=(std::min)(cap,MaxCodeShift/d);
    }
    // For canonical neutral steps, Yout slope = 1 + alpha*(modelSlope-1).
    // Retain at least 50% and at most 150% of source contrast at probe points.
    // This also bounds the clipped residual: clamping is non-expansive.
    for (unsigned variant=0;variant<4;++variant) {
        float last=0;
        for (unsigned i=0;i<=256;++i) {
            const float y=float(i)/256;
            const float g=CodeLuma(ApplyB1(lut,{y,y,y},variant));
            if (!std::isfinite(g)) return r;
            if (i) {
                const float deviation=std::abs((g-last)*256-1);
                if (deviation>0) cap=(std::min)(cap,.5f/deviation);
            }
            last=g;
        }
    }
    cap=Unit(cap); r.tone.gains={cap,cap,cap};
    double raw=0, residual=0;
    for (unsigned i=0;i<refined::SampleCount;++i) {
        RGB s{Unit(samples[3*i]),Unit(samples[3*i+1]),Unit(samples[3*i+2])};
        const auto g=ApplyB1(lut,s);
        // Diagnostic EV only, not used by the code-luma optimiser.
        const float sy=LinearLuminance(s);
        if (sy<.001f) continue;
        raw+=std::log2((std::max)(LinearLuminance(g),.0001f)/sy);
        residual+=std::log2((std::max)(LinearLuminance(Apply(s,g,cap)),.0001f)/sy);
        ++r.samples;
    }
    if (r.samples) {r.sourceShiftEv=float(raw/r.samples);r.residualEv=float(residual/r.samples);}
    // A black-only image still has valid evidence (and valid neutral probes).
    r.samples=refined::SampleCount;
    return r;
}

// Reductions are immediate; only relaxation is smoothed. Never exceed the
// newly measured safety cap. A new shot must not inherit an old constraint.
inline Tone ShotUpdate(const Tone& measured, const Tone& previous, float release)
{
    const float a=Unit(measured.gains[0]);
    const float p=Unit(previous.gains[0]);
    const float v=(std::min)(a,p+Unit(release)*(a-p));
    return Tone{{v,v,v}};
}
} // namespace svdlut::b2::luma
