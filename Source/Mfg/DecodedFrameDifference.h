// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <vector>

namespace mfg {

enum class FrameDifference { Unknown, Different, Duplicate, Cut, LowMotion };

// Compares the decoded planes before any temporal upscaling. The GPU reads
// the visible picture; the CPU reads only the reduction counters.
class DecodedFrameDifference {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Picture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> luma, chroma;
    };

    ComPtr<ID3D11Device1> device_;
    ComPtr<ID3DDeviceContextState> state_;
    ComPtr<ID3D11ComputeShader> compare_, reduce_;
    ComPtr<ID3D11Buffer> area_, tiles_, result_, readback_;
    ComPtr<ID3D11UnorderedAccessView> tilesWrite_, resultView_;
    ComPtr<ID3D11ShaderResourceView> tilesRead_;
    ComPtr<ID3D11DeviceContext4> fenceContext_;
    ComPtr<ID3D11Fence> fence_;
    uint64_t fenceValue_ = 0;
    unsigned consecutiveBusy_ = 0;
    Picture previous_, current_;
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool ready_ = false, pending_ = false, gpuAnchor_ = false, cpuAnchor_ = false;
    HRESULT readbackResult_ = S_FALSE;
    int64_t previousPts_ = -1, pendingPts_ = -1;
    uint64_t pixels_ = 0;
    std::vector<BYTE> previousCpu_, currentCpu_;
    size_t cpuPitch_ = 0, cpuHeight_ = 0;
    int64_t cpuPreviousPts_ = -1, cpuPendingPts_ = -1;
    FrameDifference cpuResult_ = FrameDifference::Unknown;

    bool Initialize(ID3D11Device1* device, const D3D11_TEXTURE2D_DESC& source);

public:
    void Reset() {
        previousPts_ = pendingPts_ = cpuPreviousPts_ = cpuPendingPts_ = -1;
        pending_ = gpuAnchor_ = cpuAnchor_ = false;
        consecutiveBusy_ = 0;
    }
    void Submit(ID3D11Device1* device, ID3D11DeviceContext1* context, ID3D11Texture2D* source,
                UINT slice, const RECT& visible, int64_t pts);
    void SubmitCpuNv12(const BYTE* source, size_t pitch, size_t height, const RECT& visible, int64_t pts,
                       bool measureLowMotion);
    FrameDifference Take(ID3D11DeviceContext1* context, int64_t pts, bool measureLowMotion);
    // Only a retained original becomes the reference for later comparisons.
    void KeepCurrent(int64_t pts);
    HRESULT ReadbackResult() const { return readbackResult_; }
};

} // namespace mfg
