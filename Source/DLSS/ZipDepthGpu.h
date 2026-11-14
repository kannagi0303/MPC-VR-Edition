// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <d3d11_4.h>
#include <filesystem>
#include <memory>

// A single asynchronous D3D12 inference job, shared by the D3D11 consumers.
class CZipDepthGpu final
{
public:
    static constexpr unsigned Size = 384;
    // Retain estimates while the MFG presenter is behind the source thread.
    static constexpr unsigned Slots = 8;
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
        Timings timings;
    };

    CZipDepthGpu();
    ~CZipDepthGpu();
    bool Start(ID3D11Device1* device, const std::filesystem::path& directory);
    void Stop();
    const wchar_t* Status(bool hasEstimate) const;
    const wchar_t* ModelPrecision() const;
    void SetSceneStart(int64_t pts);
    bool CanAccept() const;
    ID3D11RenderTargetView* Target() const;
    ID3D11ShaderResourceView* View(unsigned slot) const;
    bool Submit(ID3D11DeviceContext1* context, Result frame, SIZE picture);
    bool Take(Result& result);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
