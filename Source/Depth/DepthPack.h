// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace DepthPack {
inline constexpr char GraphFile[] = "Partition_0.bin";
inline constexpr unsigned Width = 352;
inline constexpr unsigned Height = 192;
inline constexpr unsigned Pixels = Width * Height;
inline constexpr std::size_t Limit = 256 * 1024 * 1024;
inline constexpr std::uint32_t MaxEntries = 4096;
// Align payloads for the serialized DirectML graph's 64-bit fields.
inline constexpr std::size_t Alignment = 8;
// Keep the existing 352x192 format identifier so validated resources stay reusable.
inline constexpr std::array<std::uint8_t, 8> Magic{'Z', 'D', 'D', 'M', 'L', 0, 7, 0};
using Bytes = std::vector<std::uint8_t>;
using Entries = std::map<std::string, std::span<const std::uint8_t>, std::less<>>;

inline Bytes ReadFile(const std::filesystem::path& path, std::size_t limit = Limit)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto end = stream.tellg();
    if (end <= std::streampos(0) || static_cast<std::uint64_t>(end) > limit)
        throw std::runtime_error("Missing, empty or oversized depth data file");
    Bytes bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read depth data file");
    return bytes;
}

inline bool ValidName(std::string_view name)
{
    if (name.empty() || name.size() > 255 || name.front() == '.' || !name.ends_with(".bin"))
        return false;
    for (const unsigned char c : name) {
        if (c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos)
            return false;
    }
    return true;
}

inline void ValidateHeader(std::span<const std::uint8_t> data)
{
    if (data.size() < Magic.size() || !std::ranges::equal(data.first(Magic.size()), Magic))
        throw std::runtime_error("Expected the fixed 352x192 depth pack version");
}
inline constexpr std::array<std::uint8_t, 8> WeightMagic{'Z', 'D', 'D', 'M', 'L', 'W', 1, 0};

inline Entries ReadEntries(std::span<const std::uint8_t> data, bool weightsOnly)
{
    if (data.size() > Limit) throw std::runtime_error("depth model pack is too large");
    const auto total = data.size();
    auto take = [&data](std::size_t size) {
        if (size > data.size()) throw std::runtime_error("Truncated depth model pack");
        auto result = data.first(size);
        data = data.subspan(size);
        return result;
    };
    auto read32 = [&take]() {
        auto bytes = take(4);
        return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
            std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
    };

    const auto header = take(Magic.size());
    if (weightsOnly) {
        if (!std::ranges::equal(header, WeightMagic)) throw std::runtime_error("Invalid depth weight header");
    } else ValidateHeader(header);
    const auto count = read32();
    if (!count || count > MaxEntries) throw std::runtime_error("Invalid depth model pack entry count");

    Entries entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto nameSize = read32(), dataSize = read32();
        if (!dataSize || !nameSize || nameSize > 255) throw std::runtime_error("Invalid depth entry size");
        const auto nameData = take(nameSize);
        std::string name(reinterpret_cast<const char*>(nameData.data()), nameData.size());
        const auto padding = (Alignment - (total - data.size()) % Alignment) % Alignment;
        if (!std::ranges::all_of(take(padding), [](std::uint8_t byte) { return byte == 0; }))
            throw std::runtime_error("Invalid depth model pack padding");
        if (!ValidName(name) || !entries.emplace(std::move(name), take(dataSize)).second)
            throw std::runtime_error("Invalid or duplicate depth model pack entry");
    }
    if (!data.empty() || entries.contains(GraphFile) == weightsOnly)
        throw std::runtime_error("Invalid depth model pack contents");
    return entries;
}

inline Entries Read(std::span<const std::uint8_t> data) { return ReadEntries(data, false); }

inline void Write(std::ostream& stream, const Entries& entries, bool weightsOnly = false)
{
    if (entries.empty() || entries.size() > MaxEntries || entries.contains(GraphFile) == weightsOnly)
        throw std::runtime_error("Invalid depth model pack entries");

    auto write32 = [&stream](std::uint32_t value) {
        const std::uint8_t bytes[]{std::uint8_t(value), std::uint8_t(value >> 8),
            std::uint8_t(value >> 16), std::uint8_t(value >> 24)};
        stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    };
    std::size_t total = Magic.size() + 4;
    for (const auto& [name, bytes] : entries) {
        if (!ValidName(name) || bytes.empty() || bytes.size() > Limit)
            throw std::runtime_error("Invalid or oversized depth model pack entry");
        total += 8 + name.size();
        total += (Alignment - total % Alignment) % Alignment;
        total += bytes.size();
        if (total > Limit) throw std::runtime_error("depth model pack is too large");
    }
    const auto& header = weightsOnly ? WeightMagic : Magic;
    stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    write32(static_cast<std::uint32_t>(entries.size()));
    total = Magic.size() + 4;
    for (const auto& [name, bytes] : entries) {
        write32(static_cast<std::uint32_t>(name.size()));
        write32(static_cast<std::uint32_t>(bytes.size()));
        stream.write(name.data(), name.size());
        total += 8 + name.size();
        const auto padding = (Alignment - total % Alignment) % Alignment;
        constexpr char zeros[Alignment]{};
        stream.write(zeros, padding);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        total += padding + bytes.size();
    }
    if (!stream) throw std::runtime_error("Cannot write depth model pack");
}
}
