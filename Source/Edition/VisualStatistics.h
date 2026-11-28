// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace edition::visual {
// All times are media-relative 100 ns units, not GPU completion times.
struct Range { int64_t begin = 0, end = 0; };
// Borrowed existing Presenter clock/fence, only during the actual draw.
struct PresentClock { int64_t stream = 0; uint64_t fence = UINT64_MAX; };
struct PixelRect {
    int left, top, right, bottom;
    uint32_t color;
    bool operator==(const PixelRect&) const = default;
};
struct Ranges {
    static constexpr size_t MaxSamples = 320; // full W4 lookahead slot capacity
    std::array<Range, MaxSamples> data{};
    size_t count = 0;
    void Add(int64_t begin, int64_t end, int64_t limit) noexcept {
        if (limit <= 0 || end <= begin) return;
        begin = std::clamp(begin, int64_t(0), limit);
        end = std::clamp(end, int64_t(0), limit);
        if (end <= begin || count == data.size()) return;
        data[count++] = {begin, end};
    }
    // Only coalesce overlapping/adjacent sample intervals; never fill gaps.
    void Merge() noexcept {
        std::sort(data.begin(), data.begin() + count,
            [](Range a, Range b) { return a.begin < b.begin; });
        size_t out = 0;
        for (size_t i = 0; i < count; ++i) {
            if (out && data[i].begin <= data[out-1].end)
                data[out-1].end = (std::max)(data[out-1].end, data[i].end);
            else data[out++] = data[i];
        }
        count = out;
    }
};
// Presentation-only low-pass filter. Relative coordinates are intentional:
// steady production should look stationary even as the retained PTS change.
// 250 ms is a visual time constant, not a playback or resource policy.
inline constexpr int64_t VisualTimeConstant = 2500000;
inline constexpr uint32_t TrackColor = 0xFF26313B;
inline constexpr uint32_t PreparedColor = 0xFF345E4E;
inline constexpr uint32_t RetainedColor = 0xFF75D6A4;
// Reuse the previously uploaded pixel rectangle for a one-pixel rounding
// oscillation. No history is added, and scene markers are never held.
inline void HoldPixelJitter(PixelRect& next, const PixelRect& previous) noexcept {
    if ((next.color != PreparedColor && next.color != RetainedColor) ||
        next.color != previous.color || next.top != previous.top || next.bottom != previous.bottom) return;
    const auto close = [](int a, int b) { const auto d = int64_t(a)-b; return d >= -1 && d <= 1; };
    if (close(next.left, previous.left) && close(next.right, previous.right)) next = previous;
}
inline int64_t SmoothPosition(int64_t previous, int64_t target, double alpha) noexcept {
    return previous + int64_t(double(target - previous) * alpha + (target >= previous ? 0.5 : -0.5));
}
struct VisualState {
    int64_t head = -1, limit = 0, ready = 0, queued = 0;
    std::array<Ranges, 4> rows{};
    void UpdateRow(size_t i, const Ranges& exact, bool visible, int64_t duration,
                   double alpha, bool reset) noexcept {
        auto& old = rows[i];
        if (!visible || !exact.count) { old.count = 0; return; }
        // Merge short holes only in the drawing representation. Actual PTS
        // and availability remain untouched. Large holes remain separate.
        Ranges target;
        const auto gap = std::clamp(duration, int64_t(0), limit);
        for (size_t j = 0; j < exact.count; ++j) {
            const auto r = exact.data[j];
            if (target.count && r.begin - target.data[target.count-1].end <= gap)
                target.data[target.count-1].end = r.end;
            else target.data[target.count++] = r;
        }
        if (!reset && old.count) {
            size_t first = 0;
            int64_t previousRawEnd = 0;
            for (size_t j = 0; j < target.count; ++j) {
                auto& r = target.data[j];
                const auto raw = r;
                while (first < old.count && old.data[first].end < raw.begin-gap) ++first;
                const auto priorEnd = previousRawEnd;
                previousRawEnd = raw.end;
                if (first == old.count || old.data[first].begin - raw.end > gap) continue;
                size_t last = first;
                while (last+1 < old.count && old.data[last+1].begin - raw.end <= gap) ++last;
                // A split must expose its new interior gap immediately. For
                // stable runs both edges are filtered; on a merge the outside
                // edges still inherit their previous positions.
                if (!j || old.data[first].begin > target.data[j-1].end)
                    r.begin = SmoothPosition(old.data[first].begin, raw.begin, alpha);
                if (j+1 == target.count || old.data[last].end < target.data[j+1].begin)
                    r.end = SmoothPosition(old.data[last].end, raw.end, alpha);
                r.begin = std::clamp(r.begin, int64_t(0), limit);
                r.end = std::clamp(r.end, r.begin, limit);
                if (r.end <= r.begin) r = raw;
                // Keep at least half of every large actual gap visible, even
                // when smoothing adjacent runs. Do not turn it into coverage.
                if (j) r.begin = (std::max)(r.begin, raw.begin - (raw.begin-priorEnd)/4);
                if (j+1 < target.count) r.end = (std::min)(r.end, raw.end + (target.data[j+1].begin-raw.end)/4);
                if (r.end <= r.begin) r = raw;
            }
        }
        old.count = target.count;
        std::copy_n(target.data.begin(), target.count, old.data.begin());
    }
};
struct Timeline {
    static constexpr int Columns = 61;
    static constexpr int LabelColumns = 10;
    static constexpr int Rows = 4;
    int textRow = -1;
    int64_t limit = 0, ready = 0, queued = 0;
    bool sampled = false;
    std::array<Ranges, Rows> rows{};
    std::array<bool, Rows> visible{};
    VisualState visual{};
    void BeginSnapshot() noexcept {
        // Clear only snapshot data; keep presentation history across normal
        // statistics refreshes. Existing full Timeline resets clear both.
        textRow = -1; limit = ready = queued = 0; sampled = false;
        visible.fill(false); cutCount = 0;
        for (auto& row : rows) row.count = 0;
    }
    void UpdateVisual(int64_t head, int64_t duration) noexcept {
        if (!sampled || head < 0 || limit <= 0) { visual = {}; return; }
        const bool reset = visual.head < 0 || head < visual.head || visual.limit != limit ||
            head - visual.head >= 4 * VisualTimeConstant;
        const double dt = reset ? 0.0 : double(head - visual.head);
        const double alpha = reset ? 1.0 : dt / (double(VisualTimeConstant) + dt);
        visual.head = head; visual.limit = limit;
        const auto r = std::clamp(ready, int64_t(0), limit);
        const auto q = std::clamp(queued, r, limit);
        visual.ready = reset || !r ? r : SmoothPosition(visual.ready, r, alpha);
        visual.queued = reset || !q ? q : SmoothPosition(visual.queued, q, alpha);
        visual.queued = (std::max)(visual.ready, visual.queued);
        for (size_t i = 1; i < rows.size(); ++i)
            visual.UpdateRow(i, rows[i], visible[i], duration, alpha, reset);
    }
    std::array<int64_t, 8> cuts{};
    size_t cutCount = 0;
    void AddSample(size_t row, int64_t pts, int64_t playhead, int64_t nominalDuration) noexcept {
        if (row >= rows.size() || pts < 0 || playhead < 0 || limit <= 0) return;
        // A mark represents a retained sample, NOT guaranteed availability for
        // all nearby frames. Nominal width is only the display convention.
        if (pts >= playhead && pts - playhead >= limit) return;
        const auto start = pts - playhead;
        const auto width = std::clamp(nominalDuration, int64_t(1), limit);
        if (start < -width) return;
        rows[row].Add(start, start > limit-width ? limit : start+width, limit);
    }
    void AddCut(int64_t pts, int64_t playhead) noexcept {
        if (pts < 0 || playhead < 0 || pts < playhead || pts-playhead >= limit || cutCount >= cuts.size()) return;
        const auto offset = pts-playhead;
        for (size_t i=0; i<cutCount; ++i) if (cuts[i] == offset) return;
        cuts[cutCount++] = offset;
    }
};
inline std::wstring Fit(std::wstring value, size_t width, bool right=false) {
    if (!width) return {};
    if (value.size()>width) value=value.substr(0,width-1)+L"~";
    const auto padding=std::wstring(width-value.size(),L' ');
    return right ? padding+value : value+padding;
}
inline std::wstring Field(const std::wstring& name, const std::wstring& value, size_t width, size_t labelWidth) {
    if (name.empty()) return std::wstring(width,L' ');
    labelWidth=(std::min)(labelWidth,width);
    return Fit(name,labelWidth)+Fit(value,width-labelWidth,true);
}
// Fixed text columns only. Keep the established timeline geometry unchanged.
inline std::wstring HardwareRow(const std::array<std::wstring, 4>& values) {
    return Field(L"GPU", values[0], 12, 7) + L"  " +
        Field(L"Mem Bus", values[1], 14, 9) + L"  " +
        Field(L"Decode", values[2], 12, 7) + L"  " +
        Field(L"Optical Flow", values[3], 17, 13);
}
inline std::wstring PairRow(const std::wstring& a, const std::wstring& av,
    const std::wstring& b, const std::wstring& bv, size_t labelWidth = 14) {
    if (a.empty() && b.empty()) return {};
    return Field(a, av, 29, labelWidth) + L"   " + Field(b, bv, 29, labelWidth);
}
inline std::wstring EnhancementRows(const std::array<int, 4>& strengths) {
    constexpr const wchar_t* names[] = {L"Color Enhancement", L"Memory Color", L"Adaptive Tone", L"Skin Tone"};
    std::wstring result;
    for (size_t i = 0; i < strengths.size(); i += 2) {
        const auto value = [&](size_t j) { return std::to_wstring(std::clamp(strengths[j], 0, 100)) + L"%"; };
        const auto row = PairRow(strengths[i] > 0 ? names[i] : L"", value(i),
            strengths[i+1] > 0 ? names[i+1] : L"", value(i+1), 19);
        if (!row.empty()) result += L"\n" + row;
    }
    return result;
}
inline std::wstring VideoFormatRow(unsigned width, unsigned height, unsigned srInWidth, unsigned srInHeight,
    unsigned srOutWidth, unsigned srOutHeight, const std::wstring& fps, bool hdr, const std::wstring& refresh) {
    auto left = std::to_wstring(width) + L"x" + std::to_wstring(height);
    if (width && height && srInWidth == width && srInHeight == height &&
        srOutWidth > width && srOutHeight > height)
        left += L" -> " + std::to_wstring(srOutWidth) + L"x" + std::to_wstring(srOutHeight);
    left += L"  " + fps + L" | " + (hdr ? L"HDR" : L"SDR");
    return Fit(left, 49) + Fit(refresh, 12, true);
}
inline std::wstring Axis() {
    std::wstring scale(Timeline::Columns,L' ');
    scale.replace(10,3,L"Now");
    scale.replace(20,5,L"+0.5s");
    scale.replace(33,5,L"+1.0s");
    scale.replace(46,5,L"+1.5s");
    scale.replace(58,3,L"+2s");
    return scale;
}
// AppendEditionStatistics inserts one separator row before the appended text.
// The next newline starts the first track, so the final row index is +2.
inline int FirstTrackRow(const std::wstring& throughAxis) noexcept {
    return int(std::count(throughAxis.begin(),throughAxis.end(),L'\n'))+2;
}
inline int Pixel(int64_t time, int64_t limit, int width) noexcept {
    if (limit <= 0 || width <= 0) return 0;
    return int(double(std::clamp(time, int64_t(0), limit)) * width / double(limit));
}
// Bounded rectangle emission shared by the renderer and portable regression.
// Adjacent screen-space spans also merge (sub-pixel gaps are unresolvable).
// Filtered spans may intentionally bridge short holes; rasterization itself
// only merges overlapping/adjacent pixel intervals.
template<class Rect>
void Emit(const Timeline& t, int left, int top, int right, int line, Rect rect) {
    if (!t.sampled || t.limit <= 0 || right <= left || line < 3) return;
    const int bottom=top+Timeline::Rows*line, width=right-left;
    const auto x=[&](int64_t pts){return left+Pixel(pts,t.limit,width);};
    // Raw values remain available for text; only the drawn coordinates are filtered.
    const bool filtered = t.visual.head >= 0 && t.visual.limit == t.limit;
    const auto ready = filtered ? t.visual.ready : t.ready;
    const auto queued = filtered ? t.visual.queued : t.queued;
    const int pad=(std::max)(1,line/5);
    const auto add=[&](int l,int y,int r,int b,uint32_t c){if(r>l&&b>y)rect(l,y,r,b,c);};
    for(size_t i=0;i<t.visible.size();++i){
        if(!t.visible[i])continue;
        const int lo=top+int(i)*line+pad, hi=top+int(i+1)*line-pad;
        add(left,lo,right,hi,TrackColor);
        if(!i){
            add(left,lo,x(queued),hi,PreparedColor);
            add(left,lo,x(ready),hi,RetainedColor);
        }else{
            // Dark green is final-video readiness, not per-model success.
            add(left,lo,x(ready),hi,PreparedColor);
            const auto& runs=filtered ? t.visual.rows[i] : t.rows[i];
            int first=-1,last=-1;
            for(size_t j=0;j<runs.count;++j){
                const int begin=x(runs.data[j].begin), end=(std::min)(right,(std::max)(begin+1,x(runs.data[j].end)));
                if(end<=begin)continue;
                if(first>=0 && begin<=last)last=(std::max)(last,end);
                else{if(first>=0)add(first,lo,last,hi,RetainedColor);first=begin;last=end;}
            }
            if(first>=0)add(first,lo,last,hi,RetainedColor);
        }
    }
    for(size_t i=0;i<t.cutCount;++i){const int cut=x(t.cuts[i]);add(cut,top,(std::min)(right,cut+1),bottom,0xFFFFFFFF);}
    add(left,top,left+1,bottom,0xFFDCE6EE);
    add(right-1,top,right,bottom,0xFF536373);
}
} // namespace edition::visual
