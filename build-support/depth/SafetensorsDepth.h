// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>

namespace depthrecipe {
void Generate(const std::filesystem::path& checkpoint, const std::filesystem::path& config,
    const std::filesystem::path& validatedPack, const std::filesystem::path& recipe);
void Convert(const std::filesystem::path& checkpoint, const std::filesystem::path& config,
    const std::filesystem::path& graph, const std::filesystem::path& weights);
}
