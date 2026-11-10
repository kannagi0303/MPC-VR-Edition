// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <array>
#include <memory>
#include <string>

namespace mfg {
inline constexpr unsigned MaxGenerated = 5;
inline constexpr unsigned MaxMultiplier = MaxGenerated + 1;

// Caller owns GPU synchronization: release only after submitted work completes.
// SDK definitions remain private to prevent collision with upstream's NR ABI.
class Runtime {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::wstring status_;
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    bool Open(ID3D12Device* device, const std::wstring& directory);
    bool Create(ID3D12GraphicsCommandList* list, unsigned width, unsigned height, DXGI_FORMAT format);
    bool Evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* depth,
        ID3D12Resource* motion, const std::array<ID3D12Resource*, MaxGenerated>& outputs,
        const std::array<ID3D12Resource*, MaxGenerated>& disabled, unsigned multiplier, bool reset);
    void Close();
    unsigned Maximum() const;
    const std::wstring& Status() const { return status_; }
};
}
