// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace edition::osd {
// Presentation only. FPS is a planned cadence, other effects are latched
// observations. Neither is a scheduler input or per-picture quality guarantee.
enum Flag : unsigned { Color=1, Tone=2, Memory=4, Skin=8, Flat=16 };
struct Size {
    int w=0, h=0;
    bool operator==(const Size&) const = default;
    explicit operator bool() const { return w>0 && h>0; }
};
struct Result {
    Size input{}, output{};
    int fromFps=0, toFps=0;
    unsigned flags=0;
    bool operator==(const Result&) const = default;
    bool Empty() const { return !output && !toFps && !flags; }
};
struct Request {
    int srMode=0, srLimit=0;
    unsigned mfg=0, flags=0;
    int targetFromFps=0, targetToFps=0;
    bool operator==(const Request&) const = default;
};
inline bool Upscaled(Size in, Size out) {
    return in && out && out.w>=in.w && out.h>=in.h && out!=in;
}
inline int RoundedFps(double fps) {
    return std::isfinite(fps) && fps>=1.0 && fps<=1000.0 ? int(std::lround(fps)) : 0;
}
inline void SetCadence(Result& r, double source, double refresh, unsigned multiplier) {
    if (multiplier<2 || !RoundedFps(source) || !RoundedFps(refresh)) return;
    const int from=RoundedFps(source), to=RoundedFps((std::min)(source*multiplier,refresh));
    if (to>from) { r.fromFps=from; r.toFps=to; }
}
inline std::wstring FormatSize(Size s) { return std::to_wstring(s.w)+L"x"+std::to_wstring(s.h); }
inline std::wstring MainText(const Result& r) {
    const bool scale=Upscaled(r.input,r.output), fps=r.toFps>r.fromFps && r.fromFps>0;
    std::wstring left=scale?FormatSize(r.input):L"", right=scale?FormatSize(r.output):L"";
    std::wstring from=fps?std::to_wstring(r.fromFps)+L"fps":L"", to=fps?std::to_wstring(r.toFps)+L"fps":L"";
    const auto width=(std::max)(left.size(),from.size());
    std::wstring text;
    if (scale) text=left+std::wstring(width-left.size(),L' ')+L" -> "+right;
    if (fps) {
        if (scale) text+=L'\n';
        text+=from+std::wstring(width-from.size(),L' ')+L" -> "+to;
    }
    return text;
}
inline std::wstring IndicatorText(unsigned flags) {
    std::wstring text;
    auto add=[&](unsigned bit,const wchar_t* name) { if (flags&bit) { if (!text.empty()) text+=L"  "; text+=name; } };
    add(Flat,L"2D"); add(Color,L"COLOR"); add(Tone,L"TONE"); add(Memory,L"MEMORY"); add(Skin,L"SKIN");
    return text;
}
class Notice {
public:
    static constexpr uint64_t SettleMs=600, HoldMs=5000;
    void Reset() { *this={}; }
    // Explicit settings removals apply immediately. Transient lack of evidence
    // (seek, scene cut, resize, a temporary fallback) cannot rearm the notice.
    void Observe(Request request, const Result& observed, uint64_t now) {
        if (request!=request_) {
            auto prune=[&](Result& r) {
                if (request.srMode!=request_.srMode || request.srLimit!=request_.srLimit ||
                    ((request.flags^request_.flags)&Flat)) { r.input={}; r.output={}; }
                if (request.mfg!=request_.mfg || request.targetFromFps!=request_.targetFromFps ||
                    request.targetToFps!=request_.targetToFps) { r.fromFps=0; r.toFps=0; }
                r.flags &= request.flags;
            };
            prune(ready_); prune(shown_); request_=request;
            if (shown_.Empty()) { until_=0; displaying_=false; }
        }
        Result next=ready_;
        if (!next.output && request.srMode && Upscaled(observed.input,observed.output)) {
            next.input=observed.input; next.output=observed.output;
        }
        if (!next.toFps && request.mfg && observed.toFps>observed.fromFps && observed.fromFps>0) {
            next.fromFps=observed.fromFps; next.toFps=observed.toFps;
        }
        next.flags |= observed.flags & request.flags;
        if (next!=ready_) { ready_=next; since_=now; pending_=true; }
        if (ready_==shown_) pending_=false;
        if (pending_ && now>=since_ && now-since_>=SettleMs) {
            shown_=ready_; pending_=false; displaying_=!shown_.Empty(); until_=0;
        }
    }
    Result Visible(uint64_t now) const {
        return displaying_ && (!until_ || now<until_) ? shown_ : Result{};
    }
    void Suppress() { displaying_=false; until_=0; }
    void Drawn(uint64_t now) { if (displaying_ && !until_) until_=now+HoldMs; }
private:
    Request request_{};
    Result ready_{}, shown_{};
    uint64_t since_=0, until_=0;
    bool pending_=false, displaying_=false;
};
}
