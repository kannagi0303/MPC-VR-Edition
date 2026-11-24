// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace svdlut::input {
inline constexpr unsigned Size = 256;
inline constexpr size_t PixelBytes = 4 * sizeof(float);
inline constexpr size_t MinPitch = Size * PixelBytes;

inline float Code(const uint8_t* data, size_t pitch, unsigned x, unsigned y, unsigned c)
{
    float value;
    std::memcpy(&value, data + size_t(y) * pitch + x * PixelBytes + c * sizeof(float), sizeof(value));
    return std::isfinite(value) ? (std::clamp)(value, 0.0f, 1.0f) : 0.0f;
}

// Input is RGBA32F, already in the renderer's full-range SDR code space.
// Read exactly the same values for inference, signature and B2 sample evidence.
template<class Image>
inline bool ReadPlanar(const uint8_t* data, size_t pitch, Image& image)
{
    if (!data || pitch < MinPitch || image.size() != 3 * Size * Size) return false;
    for (unsigned y = 0; y < Size; ++y)
        for (unsigned x = 0; x < Size; ++x)
            for (unsigned c = 0; c < 3; ++c)
                image[c * Size * Size + y * Size + x] = Code(data, pitch, x, y, c);
    return true;
}
} // namespace svdlut::input
