#pragma once
#include <cstddef>
#include <vector>

constexpr std::size_t Width = 384;
constexpr std::size_t Pixels = Width * Width;
constexpr int InputCount = 3;

inline std::vector<float> MakeInput(int index)
{
    std::vector<float> input(3 * Pixels);
    for (std::size_t y = 0; y < Width; ++y) {
        for (std::size_t x = 0; x < Width; ++x) {
            const auto i = y * Width + x;
            const float u = static_cast<float>(x) / (Width - 1);
            const float v = static_cast<float>(y) / (Width - 1);
            if (index == 0) {
                input[i] = u;
                input[Pixels + i] = v;
                input[2 * Pixels + i] = 0.125f + 0.75f * ((x / 32 + y / 32) % 2);
            } else if (index == 1) {
                const bool block = x >= 80 && x < 288 && y >= 96 && y < 272;
                input[i] = block ? 0.8f : u * 0.3f;
                input[Pixels + i] = block ? 0.2f : v * 0.4f;
                input[2 * Pixels + i] = block ? 0.55f : (u + v) * 0.25f;
            } else {
                const float tile = ((x / 19 + y / 23) % 3) * 0.25f;
                input[i] = 0.15f + tile;
                input[Pixels + i] = u * 0.8f;
                input[2 * Pixels + i] = (1 - v) * 0.8f;
            }
        }
    }
    return input;
}
