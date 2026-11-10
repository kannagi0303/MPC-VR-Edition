// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Session.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
#include <memory>
#include <cstdint>
namespace mfg {
struct QueuedFrame {
    int64_t deadline=0,timestamp=0;
    uint64_t serial=0;
    unsigned index=0;
    bool nativeInput=false;
    std::shared_ptr<OriginalSnapshot> original; // populated only for source frames
};
struct QueueDrops {
    unsigned real=0,generated=0;
    unsigned Total() const {return real+generated;}
    void Count(unsigned index) {if(index==MaxGenerated) ++real;else ++generated;}
};
// DirectShow PTS -> QPC deadlines. Original snapshots are independent of
// recycled NGX banks, and are never evicted by generated frames. Discontinuity
// and renderer shutdown intentionally flush this playback timeline.
class FrameQueue {
    std::vector<QueuedFrame> frames_;
    static bool Earlier(const QueuedFrame& a,const QueuedFrame& b) {
        if(a.deadline!=b.deadline) return a.deadline<b.deadline;
        if(a.timestamp!=b.timestamp) return a.timestamp<b.timestamp;
        return a.index==MaxGenerated && b.index!=MaxGenerated;
    }
public:
    void Clear() {frames_.clear();}
    unsigned PendingOriginals() const {
        return unsigned(std::count_if(frames_.begin(),frames_.end(),[](const auto& f){return f.index==MaxGenerated;}));
    }
    // Optionally ask the presentation thread to submit shortly BEFORE the PTS
    // deadline, leaving time for D3D11 composition and the next VBlank.
    // The original PTS/deadline is never modified in the queue.
    bool Next(int64_t& deadline,int64_t submitLeadTicks=0) const {
        if(frames_.empty())return false;
        deadline=frames_.front().deadline-(std::max)(int64_t{0},submitLeadTicks);
        return true;
    }
    QueueDrops Add(const Batch& b,int64_t qpc,int64_t stream,double frequency,
                   int64_t videoDelay,double refreshHz,bool nativeInput,unsigned bankCount,
                   bool autoCadence=false) {
        QueueDrops drops;
        if(!b.serial || !b.original) return drops;
        bankCount=std::clamp(bankCount,2u,3u);
        // Only generated resources belong to recyclable banks. The original
        // frame has its own ref-counted texture and must survive reuse.
        frames_.erase(std::remove_if(frames_.begin(),frames_.end(),[&](const QueuedFrame& f){
            if(f.index==MaxGenerated)return false;
            const bool expired=f.serial>=b.serial || b.serial-f.serial>=bankCount;
            if(expired)drops.Count(f.index);
            return expired;
        }),frames_.end());
        const bool clockOk=std::isfinite(frequency)&&frequency>0;
        const auto append=[&](int64_t timestamp,unsigned index) {
            // Bad clock mapping must never silently discard a source frame.
            // Put it on the immediate-present path and omit only generated work.
            int64_t deadline=qpc;
            if(clockOk) {
                const double offset=(double(timestamp)+double(videoDelay)-double(stream))*frequency/10000000.;
                if(std::isfinite(offset)&&std::abs(offset)<=frequency*2)
                    deadline=qpc+int64_t(offset);
                else if(index!=MaxGenerated) {drops.Count(index);return;}
            } else if(index!=MaxGenerated) {drops.Count(index);return;}
            // Keep generated work strictly bounded; pending source frames are
            // never removed to make room for interpolation.
            if(index!=MaxGenerated && frames_.size()>=MaxMultiplier*3) {
                drops.Count(index);return;
            }
            frames_.push_back({deadline,timestamp,b.serial,index,nativeInput,
                               index==MaxGenerated ? b.original : nullptr});
        };
        const int64_t span=b.previous>=0&&b.current>b.previous?b.current-b.previous:0;
        if(b.generated && b.generated<=MaxGenerated && span>0 && span<=2500000) {
            unsigned visible=b.generated+1;
            if(!autoCadence && std::isfinite(refreshHz)&&refreshHz>0)
                visible=std::clamp(unsigned(std::floor(refreshHz*double(span)/10000000.+.05)),1u,visible);
            for(unsigned step=1;step<visible;++step) {
                const unsigned phase=(step*(b.generated+1)+visible-1)/visible;
                append(b.previous+span*phase/(b.generated+1),phase-1);
            }
        }
        append(b.current,MaxGenerated);
        std::sort(frames_.begin(),frames_.end(),Earlier);
        return drops;
    }
    std::optional<QueuedFrame> TakeDue(int64_t qpc,QueueDrops& drops,int64_t submitLeadTicks=0) {
        // This is only an eligibility allowance for early submission; original
        // timestamps, their relative order and deadlines are preserved.
        const int64_t selectionQpc=qpc+(std::max)(int64_t{0},submitLeadTicks);
        const auto due=std::partition_point(frames_.begin(),frames_.end(),[&](const QueuedFrame& f){return f.deadline<=selectionQpc;});
        if(due==frames_.begin())return std::nullopt;
        // A due real frame always wins over generated pictures, and real frames
        // are presented in PTS order. NEVER use latest-due to replace an original.
        auto real=due;
        for(auto it=frames_.begin();it!=due;++it) {
            if(it->index==MaxGenerated && (real==due || it->timestamp<real->timestamp))
                real=it;
        }
        if(real!=due) {
            QueuedFrame selected=*real;
            frames_.erase(real);
            frames_.erase(std::remove_if(frames_.begin(),frames_.end(),[&](const QueuedFrame& f){
                if(f.index==MaxGenerated || f.timestamp>selected.timestamp)return false;
                drops.Count(f.index);return true;
            }),frames_.end());
            return selected;
        }
        // With no due original, only the freshest due interpolation is useful.
        QueuedFrame selected=*(due-1);
        for(auto it=frames_.begin();it!=due-1;++it)drops.Count(it->index);
        frames_.erase(frames_.begin(),due);
        return selected;
    }
};
} // namespace mfg
