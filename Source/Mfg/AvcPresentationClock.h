// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mfg {

// Some CFR AVC MP4s store only decode times. A decoder can deliver pictures
// in presentation order while attaching those times to the decoded pictures.
// Only enable this compatibility path for an unambiguous single video track.
class AvcPresentationClock {
    static constexpr int64_t Ticks = 10'000'000;
    static constexpr int64_t Tolerance = 10'000;
    struct Box {
        uint32_t type;
        uint64_t body;
        uint64_t end;
    };
    static constexpr uint32_t Tag(char a, char b, char c, char d) {
        return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | uint32_t(d);
    }
    static uint32_t Read32(const unsigned char* data) {
        return uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 |
               uint32_t(data[2]) << 8 | uint32_t(data[3]);
    }
    static bool Read(std::ifstream& file, uint64_t pos, unsigned char* data, size_t size) {
        return file.seekg(std::streamoff(pos)).read(reinterpret_cast<char*>(data), size).good();
    }
    static std::optional<std::vector<Box>> Children(std::ifstream& file, Box parent) {
        std::vector<Box> result;
        for (auto pos = parent.body; pos < parent.end;) {
            unsigned char data[16];
            if (result.size() >= 4096 || parent.end - pos < 8 || !Read(file, pos, data, 8))
                return std::nullopt;
            const auto type = Read32(data + 4);
            uint64_t size = Read32(data);
            uint64_t header = 8;
            if (size == 1) {
                if (parent.end - pos < 16 || !Read(file, pos + 8, data + 8, 8))
                    return std::nullopt;
                size = uint64_t(Read32(data + 8)) << 32 | Read32(data + 12);
                header = 16;
            } else if (!size) {
                size = parent.end - pos;
            }
            if (size < header || size > parent.end - pos) return std::nullopt;
            result.push_back({type, pos + header, pos + size});
            pos += size;
        }
        return result;
    }
    static std::optional<Box> Find(const std::optional<std::vector<Box>>& boxes, uint32_t type) {
        if (boxes) {
            for (const auto& box : *boxes) if (box.type == type) return box;
        }
        return std::nullopt;
    }
    static bool ReadBox(std::ifstream& file, Box box, unsigned char* data, size_t size) {
        return box.end - box.body >= size && Read(file, box.body, data, size);
    }

    int64_t m_scale = 0;
    int64_t m_interval = 0;
    int64_t m_whole = 0;
    int64_t m_remainder = 0;
    bool m_started = false;
    bool m_rejected = false;

public:
    using Interval = std::pair<int64_t, int64_t>;

    bool Available() const { return m_scale != 0; }

    void Load(const wchar_t* path) {
        *this = {};
        const std::wstring_view name(path);
        if (name.size() < 4 || _wcsicmp(name.data() + name.size() - 4, L".mp4")) return;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto end = file.tellg();
        if (end < 8) return;
        const auto top = Children(file, {0, 0, uint64_t(end)});
        if (!Find(top, Tag('f','t','y','p'))) return;
        const auto movie = Find(top, Tag('m','o','o','v'));
        if (!movie) return;
        const auto tracks = Children(file, *movie);
        if (!tracks || Find(tracks, Tag('m','v','e','x'))) return;
        int64_t scale = 0, interval = 0;
        bool haveVideo = false;
        for (const auto& track : *tracks) {
            if (track.type != Tag('t','r','a','k')) continue;
            const auto media = Find(Children(file, track), Tag('m','d','i','a'));
            if (!media) return;
            const auto mediaBoxes = Children(file, *media);
            const auto handler = Find(mediaBoxes, Tag('h','d','l','r'));
            unsigned char data[24];
            if (!handler || !ReadBox(file, *handler, data, 12)) return;
            if (Read32(data + 8) != Tag('v','i','d','e')) continue;
            if (haveVideo) return;
            haveVideo = true;
            const auto header = Find(mediaBoxes, Tag('m','d','h','d'));
            const auto info = Find(mediaBoxes, Tag('m','i','n','f'));
            if (!header || !info || !ReadBox(file, *header, data, sizeof(data)) || data[0] > 1) return;
            scale = Read32(data + (data[0] == 1 ? 20 : 12));
            const auto table = Find(Children(file, *info), Tag('s','t','b','l'));
            if (!table) return;
            const auto entries = Children(file, *table);
            if (!entries || Find(entries, Tag('c','t','t','s'))) return;
            const auto sample = Find(entries, Tag('s','t','s','d'));
            const auto timing = Find(entries, Tag('s','t','t','s'));
            if (!sample || !timing || !ReadBox(file, *sample, data, 16) || Read32(data + 4) != 1) return;
            const auto codec = Read32(data + 12);
            if (codec != Tag('a','v','c','1') && codec != Tag('a','v','c','3')) return;
            if (!ReadBox(file, *timing, data, 16) || Read32(data + 4) != 1 || Read32(data + 8) < 2) return;
            interval = int64_t(Read32(data + 12)) * Ticks;
        }
        if (scale && interval >= scale * (Ticks / 120) && interval <= scale * (Ticks / 12)) {
            m_scale = scale;
            m_interval = interval;
        }
    }

    void Reset() {
        m_started = false;
        m_rejected = false;
    }

    std::optional<Interval> Next(int64_t start, int64_t end, bool discontinuity) {
        if (discontinuity) Reset();
        if (!m_scale || m_rejected || start < 0 || end <= start) return std::nullopt;
        const auto nominal = (m_interval + m_scale / 2) / m_scale;
        if (!m_started) {
            m_whole = start;
            m_remainder = 0;
            m_started = true;
        }
        const auto current = m_whole + (m_remainder * 2 >= m_scale);
        const auto deviation = std::abs(start - current);
        const auto slots = (deviation + nominal / 2) / nominal;
        if (std::abs(end - start - nominal) > Tolerance || slots > 4 ||
            std::abs(deviation - slots * nominal) > Tolerance) {
            m_rejected = true;
            return std::nullopt;
        }
        m_whole += m_interval / m_scale;
        m_remainder += m_interval % m_scale;
        m_whole += m_remainder / m_scale;
        m_remainder %= m_scale;
        return Interval{current, m_whole + (m_remainder * 2 >= m_scale)};
    }
};

} // namespace mfg
