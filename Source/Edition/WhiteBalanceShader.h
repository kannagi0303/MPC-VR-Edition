// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// [WB-06 | 2026-11-25 12:55 +08:00] Same linear-light mapping in model
// thumbnail and final colour shader. Exact bypass when disabled/identity.
inline constexpr char WhiteBalanceShader[] = R"(
float3 ApplyWhiteBalance(float3 rgb, float4 control) {
    if(control.w<=0.0) return rgb;
    const float3 w=float3(.2126,.7152,.0722);
    float3 x=saturate(rgb);
    x=lerp(x/12.92,pow((x+.055)/1.055,2.4),step(.04045,x));
    float y=dot(x,w);
    float3 q=x*control.xyz;
    float z=dot(q,w);
    if(z<=1e-10) return rgb;
    float3 c=q*(y/z)-y;
    float hi=max(max(c.r,c.g),max(c.b,1e-10));
    float lo=max(max(-c.r,-c.g),max(-c.b,1e-10));
    float3 v=saturate(y+c*saturate(min((1-y)/hi,y/lo)));
    return saturate(lerp(v*12.92,1.055*pow(v,1.0/2.4)-.055,step(.0031308,v)));
}
)";
