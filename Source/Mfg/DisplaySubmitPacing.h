// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mfg {
// DXGI Present(1) submits work for a future refresh; waking the CPU only on
// the PTS deadline can miss that refresh even if the frame is GPU-ready.
// This adjusts the *submit* target, not the media PTS or A/V clock. It is
// deliberately bounded to <= one quarter of a refresh period.
class DisplaySubmitPacing {
    static constexpr std::size_t Window=96;
    static constexpr std::size_t Warmup=8;
    std::array<double,Window> desired_{};
    std::size_t count_=0, next_=0;
    double leadMs_=0, p95NeedMs_=0;
public:
    void Reset() {count_=next_=0;leadMs_=p95NeedMs_=0;}
    std::size_t Samples() const {return count_;}
    double LeadMs() const {return leadMs_;}
    double P95NeedMs() const {return p95NeedMs_;}
    static double MaxLeadMs(double refreshHz) {
        if(!std::isfinite(refreshHz) || refreshHz<24.0 || refreshHz>1000.0)
            return 0.0;
        return std::min(4.0,250.0/refreshHz); // at most 1/4 refresh
    }
    double Start(double refreshHz) {
        const double limit=MaxLeadMs(refreshHz);
        if(limit==0) {Reset();return 0;}
        if(count_==0 && leadMs_==0) leadMs_=std::min(1.5,limit);
        leadMs_=std::clamp(leadMs_,0.0,limit);
        return leadMs_;
    }
    // signed lateness of a SELECTED frame, in ms: actual queue extraction QPC
    // minus the true PTS deadline QPC. Does not assume Present is scan-out.
    double Observe(double latenessMs,double refreshHz) {
        const double limit=MaxLeadMs(refreshHz);
        if(limit==0) {Reset();return 0;}
        if(!std::isfinite(latenessMs)) return leadMs_;
        Start(refreshHz);
        // Exclude abnormal stalls (>2 refresh periods), e.g. seek or focus
        // transitions, so one pause cannot permanently bias the controller.
        if(std::abs(latenessMs)>2000.0/refreshHz) return leadMs_;
        // Estimate the submission lead required for a 1ms pre-deadline margin.
        // Adding current lead avoids feedback oscillation when the early wake
        // succeeds and lateness becomes negative.
        const double needed=std::clamp(leadMs_+latenessMs+1.0,0.0,limit);
        desired_[next_]=needed;
        next_=(next_+1)%Window;
        count_=std::min(count_+1,Window);
        if(count_<Warmup) return leadMs_;
        auto sorted=desired_;
        std::sort(sorted.begin(),sorted.begin()+count_);
        const std::size_t at=std::min(count_-1,static_cast<std::size_t>(std::ceil(.95*double(count_-1))));
        p95NeedMs_=sorted[at];
        const double delta=p95NeedMs_-leadMs_;
        if(delta>.20) leadMs_+=std::min(delta,.50);
        else if(delta<-.20) leadMs_+=std::max(delta,-.05);
        leadMs_=std::clamp(leadMs_,0.0,limit);
        return leadMs_;
    }
};
} // namespace mfg
