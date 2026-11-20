// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace edition {

inline std::wstring CompactNumber(double value)
{
    auto text = std::format(L"{:.2f}", value);
    if (text.find(L'.') != std::wstring::npos) {
        while (text.back() == L'0') text.pop_back();
        if (text.back() == L'.') text.pop_back();
    }
    return text;
}

inline std::wstring ScalingStatistics(int inWidth, int inHeight, int outWidth, int outHeight)
{
    if (inWidth <= 0 || inHeight <= 0 || outWidth <= 0 || outHeight <= 0) return L"Off";
    if (inWidth == outWidth && inHeight == outHeight) return std::format(L"On | {}x{}", inWidth, inHeight);

    auto text = std::format(L"{}x{} -> {}x{}", inWidth, inHeight, outWidth, outHeight);
    const auto widthScale = CompactNumber(double(outWidth) / inWidth);
    const auto heightScale = CompactNumber(double(outHeight) / inHeight);
    if (widthScale == heightScale && widthScale != L"1") text += std::format(L" (x{})", widthScale);
    return text;
}

inline unsigned BufferPercent(int64_t ready, int64_t target)
{
    if (ready <= 0 || target <= 0) return 0;
    if (ready >= target) return 100;
    return (std::min)(99u, unsigned(double(ready) * 100 / double(target)));
}

struct BufferLowWatermark {
    static constexpr uint64_t SampleIntervalMs = 250;
    static constexpr uint64_t WindowMs = 10'000;

    void Add(uint64_t tick, unsigned percent) {
        samples[next] = {tick, percent};
        next = (next + 1) % samples.size();
    }

    std::optional<unsigned> Minimum(uint64_t now) const {
        std::optional<unsigned> lowest;
        for (const auto& sample : samples) {
            if (sample.tick && sample.tick <= now && now - sample.tick < WindowMs &&
                (!lowest || sample.percent < *lowest)) lowest = sample.percent;
        }
        return lowest;
    }

private:
    struct Sample { uint64_t tick = 0; unsigned percent = 0; };
    std::array<Sample, WindowMs / SampleIntervalMs + 1> samples{};
    std::size_t next = 0;
};

struct PreDrawWindow {
    std::bitset<60> frames;
    std::size_t count = 0;
    unsigned playbackRun = 0;

    void Observe(unsigned run, bool reused) {
        if (playbackRun != run) *this = {};
        playbackRun = run;
        frames <<= 1;
        frames.set(0, reused);
        count = (std::min)(count + 1, frames.size());
    }
};

} // namespace edition
