// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdio>

namespace mfg {

// Opt-in, sparsely sample the raw OFA and submitted DLSS-G inputs by bank.
// No readback runs in the presenter, and a full staging ring drops samples.
class LetterboxTrace {
    using View = ID3D11ShaderResourceView;
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Sample {
        ComPtr<ID3D11Texture2D> staging;
        int64_t previous = 0, current = 0;
        uint64_t serial = 0;
        unsigned bank = 0;
        RECT source{}, visible{}, crop{}, present{};
        SIZE input{}, work{};
        bool pending = false;
    };
    std::array<Sample, 4> samples_;
    ComPtr<ID3D11Texture2D> result_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11PixelShader> shader_;
    FILE* file_ = nullptr;

    static constexpr char Shader[] = R"(
Texture2D<float4> Original : register(t0);
Texture2D<int2> Raw : register(t1);
Texture2D<float2> Submitted : register(t2);
Texture2D<float4> Color : register(t3);
Texture2D<int4> Candidate : register(t5);
Texture2D<int4> Active : register(t6);
cbuffer Params : register(b0) {uint2 size;uint2 inputSize;uint grid;uint flags;uint2 unused;int4 picture;};
float Level(float3 c) { return max(c.r, max(c.g, c.b)); }
float4 Main(float4 pos : SV_POSITION) : SV_TARGET {
    int region = int(pos.y);
    const int4 active = Active.Load(int3(0, 0, 0));
    const int3 candidate = Candidate.Load(int3(0, 0, 0)).xyz;
    int top = active.y, bottom = active.w;
    if (candidate.z >= 3) {
        top = picture.y + candidate.x;
        bottom = picture.w - candidate.y;
    }
    if (region == 5) return pos.x < 1.0 ? float4(active) : float4(top, bottom, candidate.z, 0);
    int first, last;
    switch (region) {
        case 0: first = max(0, top / 2 - 4); last = min(top, top / 2 + 4); break;
        case 1: first = max(0, top - 4); last = min(int(size.y), top + 4); break;
        case 2: first = (top + bottom) / 2 - 4; last = first + 8; break;
        case 3: first = max(0, bottom - 4); last = min(int(size.y), bottom + 4); break;
        default:
            first = max(bottom, (bottom + int(size.y)) / 2 - 4);
            last = min(int(size.y), (bottom + int(size.y)) / 2 + 4);
            break;
    }
    if (first >= last || (top == 0 && region == 1) || (bottom == size.y && region == 3))
        return -1;
    float4 sums = 0;
    float2 peaks = 0;
    [loop] for (int i = 0; i < 128; ++i) {
        const int2 p = int2(picture.x + (2 * (i % 16) + 1) * (picture.z - picture.x) / 32,
            first + (i / 16) * (last - first) / 8);
        if (pos.x < 1.0) {
            const float raw = length(float2(Raw.Load(int3(uint2(p) / grid, 0))) / 32.0);
            const float submitted = length(Submitted.Load(int3(p, 0)));
            sums += float4(raw > 0, submitted > 0, raw, submitted);
        } else {
            const int2 src = int2((float2(p) + 0.5) * float2(inputSize) / float2(size));
            const float2 level = float2(Level(Original.Load(int3(src, 0)).rgb), Level(Color.Load(int3(p, 0)).rgb));
            sums.zw += level;
            peaks = max(peaks, level);
        }
    }
    return float4(pos.x < 1.0 ? sums.xy / 128.0 : peaks, sums.zw / 128.0);
}
)";

    void Poll(ID3D11DeviceContext* context) {
        static constexpr const char* regions[]{"top_bar", "top_edge", "center", "bottom_edge", "bottom_bar"};
        for (auto& sample : samples_) {
            if (!sample.pending) continue;
            D3D11_MAPPED_SUBRESOURCE data{};
            const HRESULT hr = context->Map(sample.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            sample.pending = false;
            if (FAILED(hr)) continue;
            const auto row = [&](unsigned y) { return reinterpret_cast<const float*>(static_cast<const BYTE*>(data.pData) + y * data.RowPitch); };
            const auto* bounds = row(5);
            for (unsigned i = 0; i < std::size(regions); ++i) {
                const auto* values = row(i);
                std::fprintf(file_, "%lld,%lld,%llu,%u,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,"
                    "%ld,%ld,%ld,%ld,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n",
                    (long long)sample.previous, (long long)sample.current, (unsigned long long)sample.serial, sample.bank, regions[i],
                    sample.crop.left, sample.crop.top, sample.crop.right, sample.crop.bottom,
                    sample.source.left, sample.source.top, sample.source.right, sample.source.bottom,
                    sample.present.left, sample.present.top, sample.present.right, sample.present.bottom,
                    sample.input.cx, sample.input.cy, sample.work.cx, sample.work.cy,
                    sample.visible.left, sample.visible.top, sample.visible.right, sample.visible.bottom,
                    bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5], bounds[6],
                    values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]);
            }
            context->Unmap(sample.staging.Get(), 0);
        }
    }

public:
    ~LetterboxTrace() { if (file_) std::fclose(file_); }
    bool Enabled() const { return file_ != nullptr; }

    void Init(ID3D11Device* device) {
        wchar_t path[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"MPCVR_MFG_LETTERBOX_TRACE", path, _countof(path));
        if (!length || length >= _countof(path)) return;
        ComPtr<ID3DBlob> code;
        if (FAILED(D3DCompile(Shader, sizeof(Shader) - 1, nullptr, nullptr, nullptr, "Main", "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_))) return;
        D3D11_TEXTURE2D_DESC desc{2, 6, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &result_)) ||
            FAILED(device->CreateRenderTargetView(result_.Get(), nullptr, &target_))) return;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& sample : samples_)
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &sample.staging))) return;
        if (_wfopen_s(&file_, path, L"ab") || !file_) return;
        std::fseek(file_, 0, SEEK_END);
        if (std::ftell(file_) == 0) std::fprintf(file_,
            "previous_pts_100ns,current_pts_100ns,serial,bank,region,decode_l,decode_t,decode_r,decode_b,"
            "source_l,source_t,source_r,source_b,present_l,present_t,present_r,present_b,input_w,input_h,work_w,work_h,"
            "visible_l,visible_t,visible_r,visible_b,active_l,active_t,active_r,active_b,probe_top,probe_bottom,confirmations,"
            "raw_nonzero_fraction,submitted_nonzero_fraction,raw_mean_px,submitted_mean_px,source_rgb_max,work_rgb_max,source_rgb_mean,work_rgb_mean\n");
    }

    void Submit(ID3D11DeviceContext* context, int64_t previous, int64_t current, uint64_t serial, unsigned bank,
        RECT source, RECT crop, RECT present, RECT visible, SIZE input, SIZE work,
        View* original, View* raw, View* submitted, View* color, View* candidate, View* active) {
        Poll(context);
        for (auto& sample : samples_) {
            if (sample.pending) continue;
            const D3D11_VIEWPORT viewport{0, 0, 2, 6, 0, 1};
            context->RSSetViewports(1, &viewport);
            View* inputs[]{original, raw, submitted, color, nullptr, candidate, active};
            context->PSSetShaderResources(0, UINT(std::size(inputs)), inputs);
            context->PSSetShader(shader_.Get(), nullptr, 0);
            context->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
            context->Draw(3, 0);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            View* empty[std::size(inputs)]{};
            context->PSSetShaderResources(0, UINT(std::size(empty)), empty);
            context->CopyResource(sample.staging.Get(), result_.Get());
            sample = {sample.staging, previous, current, serial, bank, source, visible, crop, present, input, work, true};
            break;
        }
    }
};

} // namespace mfg
