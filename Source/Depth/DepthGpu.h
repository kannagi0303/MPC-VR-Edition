// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <d3d11_4.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

// Serial D3D12 inference, with optional overlapping RAW capture and publication.
class CDepthGpu final
{
public:
    // Retain estimates while the MFG presenter is behind the source thread.
    static constexpr unsigned NormalSlots = 8;
    static constexpr unsigned Slots = 320;
    struct Timings {
        double packMs = 0;
        double modelMs = 0;
        double postMs = 0;
        bool available = false;
    };
    struct Result {
        int64_t pts = -1;
        uint64_t generation = 0;
        unsigned slot = 0;
        double inferenceMs = 0;
        bool valid = false;
        bool sceneCut = false;
        Timings timings;
    };

    CDepthGpu();
    ~CDepthGpu();
    bool Start(ID3D11Device1* device, const std::filesystem::path& directory);
    void Stop(bool fastShutdown = false);
    const wchar_t* Status(bool hasEstimate) const;
    bool Loading() const;
    void SetSceneStart(int64_t pts);
    bool CanAccept() const;
    bool CanStart() const;
    unsigned Capacity() const;
    SIZE ModelSize() const;
    bool SlotBusy(unsigned slot) const;
    ID3D11RenderTargetView* Target(unsigned& bank) const;
    ID3D11ShaderResourceView* View(unsigned slot) const;
    bool Submit(ID3D11DeviceContext1* context, Result frame, unsigned bank);
    bool Take(Result& result);

private:
    struct Impl;
    static std::mutex s_retiredMutex;
    static std::vector<std::unique_ptr<Impl>> s_retired;
    static void ReapRetired();
    static void Retire(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
