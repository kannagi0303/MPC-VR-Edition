// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

class ZipDepthNative final {
public:
    ZipDepthNative(ID3D12Device* device, ID3D12CommandQueue* queue,
        ID3D12Resource* image, ID3D12Resource* depth, std::span<const std::uint8_t> model,
        const std::filesystem::path& directml);
    ~ZipDepthNative();

    void Record(ID3D12GraphicsCommandList* list) const;
    bool UsesFp16() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
