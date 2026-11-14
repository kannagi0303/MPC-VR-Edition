// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace mfg {

enum class FlowMotion { Unknown, Quiet, Active };

// A bounded readback of two counters from an already-computed NVOFA field.
// Readers only use results for the requested source PTS and never wait.
class FlowMotionCheck {
    struct Sample {
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
        int64_t pts = -1;
        uint64_t cells = 0;
    };

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> reduce_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> result_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> resultView_;
    std::array<Sample, 2> samples_{};
    size_t next_ = 0;
    bool tried_ = false;

    bool Initialize(ID3D11Device* device);

public:
    void Reset();
    void Submit(ID3D11Device* device, ID3D11DeviceContext* context,
                ID3D11ShaderResourceView* vectors, UINT width, UINT height, int64_t pts);
    FlowMotion Take(ID3D11DeviceContext* context, int64_t pts);
};

} // namespace mfg
