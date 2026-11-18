// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>

struct IDXGIAdapter;

namespace svdlut {

constexpr unsigned InputSize = 256;
constexpr unsigned InputPixels = InputSize * InputSize;
constexpr unsigned LutSize = 33;
constexpr unsigned GridSize = 17;
constexpr unsigned LutLayers = 4 * 3; // Four edge combinations, with three color-pair planes each.
using Image = std::array<float, 3 * InputPixels>;
HRESULT CreateGradeShader(ID3D11Device* device, ID3D11PixelShader** shader);

struct Parameters {
    // R/G/B output channels in RGB slots. The fourth slot is padding.
    using Texel = std::array<float, 4>;
    std::array<Texel, LutLayers * LutSize * LutSize> lut;
};

class ModelWorker {
public:
    enum class State { Idle, Loading, Ready, Failed };

    ModelWorker();
    ~ModelWorker();
    bool Start(IDXGIAdapter* adapter, ID3D11Device* renderDevice, const std::filesystem::path& directory);
    void Stop(bool fast);
    void Invalidate();
    bool ReadyForInput() const;
    bool Submit(std::unique_ptr<Image> image);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TakeResult();
    ID3D11PixelShader* Shader() const;
    State GetState() const;
    std::wstring Failure() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace svdlut
