// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Edition/FixedShaderBytecode.h"
#include "MotionShaders.h"

namespace mfg {

struct PictureTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;

    HRESULT Create(ID3D11Device* device, UINT height, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Height = height;
        desc.Format = format;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture.Get(), nullptr, &view);
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(texture.Get(), nullptr, &target);
        return hr;
    }
};

// Each frame classifies its own rows on the GPU. Only the candidate and its
// confirmation count persist, so a seek, fade or aspect change cannot apply
// an old rectangle to the current frame while waiting for a CPU readback.
class Letterbox {
    PictureTexture rows_;
    PictureTexture history_[2];
    Microsoft::WRL::ComPtr<ID3D11PixelShader> rowsShader_, pictureShader_;
    unsigned slot_ = 0;
    bool initialized_ = false;

    template<edition::FixedShaderString Entry>
    static HRESULT Compile(ID3D11Device* device, ID3D11PixelShader** shader) {
        Microsoft::WRL::ComPtr<ID3DBlob> code;
        HRESULT hr = edition::CompileFixedShader<MotionShaders, Entry, "ps_5_0">(&code);
        return FAILED(hr) ? hr : device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader);
    }

public:
    HRESULT Init(ID3D11Device* device, UINT height) {
        HRESULT hr = rows_.Create(device, height, DXGI_FORMAT_R16G16_FLOAT);
        for (auto& history : history_)
            if (SUCCEEDED(hr)) hr = history.Create(device, 1, DXGI_FORMAT_R32G32B32A32_SINT);
        if (SUCCEEDED(hr)) hr = Compile<"PictureRows">(device, &rowsShader_);
        if (SUCCEEDED(hr)) hr = Compile<"ActivePicture">(device, &pictureShader_);
        return hr;
    }

    ID3D11ShaderResourceView* Draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* color,
        const PictureTexture& active, UINT height, bool detect, bool reset) {
        if (detect) {
            const D3D11_VIEWPORT viewport{0, 0, 1, float(height), 0, 1};
            context->RSSetViewports(1, &viewport);
            auto* target = rows_.target.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->PSSetShaderResources(0, 1, &color);
            context->PSSetShader(rowsShader_.Get(), nullptr, 0);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
        }
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        context->RSSetViewports(1, &viewport);
        const auto& prior = history_[slot_];
        slot_ ^= 1;
        ID3D11RenderTargetView* targets[]{active.target.Get(), history_[slot_].target.Get()};
        ID3D11ShaderResourceView* inputs[]{rows_.view.Get(), initialized_ && !reset ? prior.view.Get() : nullptr};
        context->PSSetShaderResources(4, 2, inputs);
        context->OMSetRenderTargets(2, targets, nullptr);
        context->PSSetShader(pictureShader_.Get(), nullptr, 0);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* empty[2]{};
        context->PSSetShaderResources(4, 2, empty);
        initialized_ = true;
        return active.view.Get();
    }

    ID3D11ShaderResourceView* Candidate() const { return history_[slot_].view.Get(); }
};

} // namespace mfg
