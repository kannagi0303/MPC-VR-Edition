// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "ZipDepth.h"
#include "NgxRuntime.h"
#include "Mfg/MotionShaders.h"
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr char CaptureShader[] = R"(
Texture2D<float4> Source : register(t0);
SamplerState Linear : register(s0);
cbuffer Settings : register(b0) {
    float4 Picture; // normalized origin and extent in the source texture
    float4 Model;   // content scale in the square model input, input size
    float4 Padding;
};
float4 Main(float4 pos : SV_POSITION) : SV_TARGET {
    float2 uv = saturate((pos.xy / Model.z - 0.5) / Model.xy + 0.5);
    uint w, h;
    Source.GetDimensions(w, h);
    float2 halfPixel = 0.5 / float2(w, h);
    float2 sampleUV = clamp(Picture.xy + uv * Picture.zw,
        Picture.xy + halfPixel, Picture.xy + Picture.zw - halfPixel);
    return float4(saturate(Source.SampleLevel(Linear, sampleUV, 0).rgb), 1.0);
}
)";

constexpr char TemporalShader[] = R"(
Texture2D<float> NewDepth : register(t0);
Texture2D<float> History : register(t1);
Texture2D<float2> Motion : register(t2);
SamplerState Linear : register(s0);
cbuffer Settings : register(b0) {
    float4 Picture; // origin and extent, in output pixels
    float4 Model;   // content scale, has new estimate, has history
    float4 Frame;   // output width and height, estimate age in frames, reverse Z
};
float Main(float4 pos : SV_POSITION) : SV_TARGET {
    float2 uv = (pos.xy - Picture.xy) / Picture.zw;
    if (any(uv < 0.0) || any(uv >= 1.0))
        return 0.5;

    // Both callers provide current-to-previous vectors in output pixels.
    float2 mv = Motion.Load(int3(uint2(pos.xy), 0));
    float2 prior = pos.xy + mv;
    bool inPicture = all(prior >= Picture.xy) && all(prior < Picture.xy + Picture.zw);
    prior = clamp(prior, Picture.xy + 0.5, Picture.xy + Picture.zw - 0.5);
    float depth = Model.w > 0.5 && inPicture ? History.SampleLevel(Linear, prior / Frame.xy, 0) : 0.5;
    depth = lerp(depth, 0.5, 0.005);
    if (Model.z > 0.5) {
        float2 aligned = uv + mv * min(Frame.z, 4.0) / Picture.zw;
        if (all(aligned >= 0.0) && all(aligned < 1.0)) {
            float2 square = (aligned - 0.5) * Model.xy + 0.5;
            float estimate = NewDepth.SampleLevel(Linear, square, 0);
            if (Frame.w > 0.5)
                estimate = 1.0 - estimate;
            depth = lerp(depth, estimate, Model.w > 0.5 ? 0.30 : 1.0);
        }
    }
    return clamp(depth, 0.44, 0.56);
}
)";

bool CreatePixelShader(ID3D11Device1* device, const char* text, size_t size, ID3D11PixelShader** shader)
{
    ComPtr<ID3DBlob> code;
    return SUCCEEDED(D3DCompile(text, size, nullptr, nullptr, nullptr, "Main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), nullptr)) &&
        SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader));
}

bool CreateInputs(ID3D11Device1* device,
    ID3D11SamplerState** sampler, ID3D11Buffer** constants)
{
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 48;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateSamplerState(&desc, sampler)) &&
        SUCCEEDED(device->CreateBuffer(&buffer, nullptr, constants));
}

std::array<float, 2> ContentScale(SIZE size)
{
    const float side = float((std::max)(size.cx, size.cy));
    return {float(size.cx) / side, float(size.cy) / side};
}

SIZE DepthPictureSize(SIZE modelSize)
{
    constexpr LONG side = CZipDepth::Size;
    const LONG longest = (std::max)(modelSize.cx, modelSize.cy);
    return {(std::max)(1, MulDiv(side, modelSize.cx, longest)),
            (std::max)(1, MulDiv(side, modelSize.cy, longest))};
}
}

bool CZipDepth::Initialize(ID3D11Device1* device)
{
    m_device = device;
    const auto featureLevel = device->GetFeatureLevel();
    D3D_FEATURE_LEVEL chosen{};
    ComPtr<ID3DBlob> vertex;
    if (FAILED(device->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device1), &chosen, m_state.GetAddressOf())) ||
        FAILED(D3DCompile(mfg::MotionShaders, sizeof(mfg::MotionShaders) - 1,
            nullptr, nullptr, nullptr, "VS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, vertex.GetAddressOf(), nullptr)) ||
        FAILED(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
            nullptr, m_vertex.GetAddressOf())) ||
        !CreatePixelShader(device, CaptureShader, sizeof(CaptureShader) - 1, m_captureShader.GetAddressOf()) ||
        !CreatePixelShader(device, TemporalShader, sizeof(TemporalShader) - 1, m_temporalShader.GetAddressOf()) ||
        !CreateInputs(device, m_sampler.GetAddressOf(), m_constants.GetAddressOf()))
        return false;

    const auto directory = std::filesystem::path(NgxRuntime::EditionDir());
    return m_gpu.Start(device, directory);
}

void CZipDepth::Prepare(ID3D11Device1* device)
{
    if (m_device.Get() == device) return;
    if (m_device) Stop();
    m_initialized = Initialize(device);
}

void CZipDepth::RecordResultAge(int64_t age, uint64_t now)
{
    m_counters.resultAge = age;
    if (age <= MaxEstimateAge) {
        m_consecutiveLate = 0;
        m_retryAt = 0;
        return;
    }
    ++m_counters.lateMaps;
    // Do not keep spending GPU time at the normal capture rate when the
    // complete capture-to-result path repeatedly misses the deadline.
    // Probe again after the load has had time to change.
    constexpr unsigned lateLimit = 3;
    constexpr uint64_t retryDelayMs = 5000;
    if (m_consecutiveLate < lateLimit) ++m_consecutiveLate;
    if (m_consecutiveLate == lateLimit) m_retryAt = now + retryDelayMs;
}

void CZipDepth::Update(ID3D11Device1* device, ID3D11DeviceContext1* context,
    ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts)
{
    if (!source || pts < 0 || modelSize.cx <= 0 || modelSize.cy <= 0)
        return;
    Prepare(device);
    if (!m_initialized) return;
    // MFG may confirm a cut after SR already tried to capture that source PTS.
    if (pts == m_lastSourcePts && (pts != m_sceneCuts.back() || pts == m_lastSubmittedPts)) return;
    // One capture per source PTS across the SR and MFG call sites.
    if (m_modelSize.cx != modelSize.cx || m_modelSize.cy != modelSize.cy ||
        pts < m_lastSourcePts || (m_lastSourcePts >= 0 && pts - m_lastSourcePts > 2'500'000)) {
        ResetHistory();
        m_modelSize = modelSize;
    }
    m_lastSourcePts = pts;

    PollCompleted();
    if (CoolingDown(GetTickCount64()) || !m_gpu.CanAccept()) return;

    ComPtr<ID3D11Resource> resource;
    source->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (picture.left < 0 || picture.top < 0 || picture.right > LONG(desc.Width) ||
        picture.bottom > LONG(desc.Height) || picture.right <= picture.left || picture.bottom <= picture.top)
        return;

    const auto scale = ContentScale(modelSize);
    const float data[12]{float(picture.left) / desc.Width, float(picture.top) / desc.Height,
        float(picture.right - picture.left) / desc.Width, float(picture.bottom - picture.top) / desc.Height,
        scale[0], scale[1], float(CZipDepth::Size), 0};
    constexpr float side = float(CZipDepth::Size);
    const D3D11_VIEWPORT viewport{0, 0, side, side, 0, 1};
    ComPtr<ID3DDeviceContextState> previous;
    context->SwapDeviceContextState(m_state.Get(), previous.GetAddressOf());
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertex.Get(), nullptr, 0);
    context->PSSetShader(m_captureShader.Get(), nullptr, 0);
    auto* constants = m_constants.Get();
    auto* sampler = m_sampler.Get();
    auto* target = m_gpu.Target();
    context->UpdateSubresource(constants, 0, nullptr, data, 0, 0);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetShaderResources(0, 1, &source);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* empty = nullptr;
    context->PSSetShaderResources(0, 1, &empty);
    context->SwapDeviceContextState(previous.Get(), nullptr);
    const auto slot = unsigned(std::min_element(m_resultPts.begin(), m_resultPts.end()) - m_resultPts.begin());
    if (m_gpu.Submit(context, {pts, m_generation, slot}, DepthPictureSize(modelSize))) {
        // Stop advertising this slot before the GPU can overwrite it. The
        // D3D11 fence also retires its earlier uses on this immediate context.
        m_resultPts[slot] = -1;
        m_lastSubmittedPts = pts;
        ++m_counters.captures;
        ++m_counters.jobs;
    }
}

void CZipDepth::PollCompleted()
{
    if (!m_initialized) return;
    const auto now = GetTickCount64();
    CZipDepthGpu::Result estimate;
    if (m_gpu.Take(estimate) && estimate.valid &&
        estimate.generation == m_generation && estimate.pts <= m_lastSourcePts) {
        m_counters.inferenceMs = estimate.inferenceMs;
        m_counters.gpuTimings = estimate.timings;
        m_resultPts[estimate.slot] = estimate.pts;
        ++m_counters.maps;
        if (estimate.pts >= SceneStart(m_lastSourcePts))
            RecordResultAge(m_lastSourcePts - estimate.pts, now);
    }
    if (!m_mapRateTick) {
        m_mapRateTick = now;
        m_mapRateBase = m_counters.maps;
    } else if (now - m_mapRateTick >= 1000) {
        m_counters.mapsPerSecond = double(m_counters.maps - m_mapRateBase) * 1000.0 / double(now - m_mapRateTick);
        m_mapRateTick = now;
        m_mapRateBase = m_counters.maps;
    }
}

CZipDepth::Estimate CZipDepth::ForFrame(int64_t pts) const
{
    const auto estimate = EstimateAtOrBefore(pts);
    return estimate.view && estimate.pts >= SceneStart(pts) && pts - estimate.pts <= MaxEstimateAge
        ? estimate : Estimate{};
}

CZipDepth::Estimate CZipDepth::LatestPreview() const
{
    // Diagnostics show the newest completed result independently of the presenter.
    return EstimateAtOrBefore(INT64_MAX);
}

CZipDepth::Estimate CZipDepth::EstimateAtOrBefore(int64_t pts) const
{
    Estimate estimate;
    for (unsigned slot = 0; slot < m_resultPts.size(); ++slot) {
        const auto resultPts = m_resultPts[slot];
        if (resultPts > estimate.pts && resultPts <= pts)
            estimate = {m_gpu.View(slot), resultPts, m_modelSize};
    }
    return estimate;
}

int64_t CZipDepth::SceneStart(int64_t pts) const
{
    const auto cut = std::upper_bound(m_sceneCuts.begin(), m_sceneCuts.end(), pts);
    return cut == m_sceneCuts.begin() ? -1 : *std::prev(cut);
}

void CZipDepth::MarkSceneCut(int64_t pts)
{
    const auto cut = std::lower_bound(m_sceneCuts.begin(), m_sceneCuts.end(), pts);
    if (cut == m_sceneCuts.begin() || (cut != m_sceneCuts.end() && *cut == pts)) return;

    // Older estimates remain available to originals already queued for presentation.
    std::move(m_sceneCuts.begin() + 1, cut, m_sceneCuts.begin());
    *std::prev(cut) = pts;
    if (cut == m_sceneCuts.end()) {
        m_gpu.SetSceneStart(pts);
        m_consecutiveLate = 0;
        m_retryAt = 0;
    }
}

void CZipDepth::ResetHistory()
{
    ++m_generation;
    m_lastSourcePts = m_lastSubmittedPts = -1;
    m_resultPts.fill(-1);
    m_sceneCuts.fill(-1);
    m_gpu.SetSceneStart(-1);
}

void CZipDepth::Stop()
{
    m_gpu.Stop();
    ResetHistory();
    m_device.Reset();
    m_state.Reset();
    m_vertex.Reset();
    m_captureShader.Reset();
    m_temporalShader.Reset();
    m_sampler.Reset();
    m_constants.Reset();
    m_counters = {};
    m_mapRateTick = 0;
    m_mapRateBase = 0;
    m_consecutiveLate = 0;
    m_retryAt = 0;
    m_initialized = false;
}

const wchar_t* CZipDepth::Status() const
{
    if (!m_device) return L"awaiting source";
    if (!m_initialized) return L"DirectML unavailable";
    return m_gpu.Status(LatestPreview().view != nullptr);
}

bool CZipDepthHistory::Draw(ID3D11Device1* device, ID3D11DeviceContext1* context,
    const CZipDepth& source, ID3D11Texture2D* output, ID3D11RenderTargetView* target,
    ID3D11ShaderResourceView* motion, const RECT& picture, SIZE modelSize,
    int64_t pts, float frameTimeMs, bool inverted)
{
    if (m_failed || !source.m_initialized || pts < 0 || modelSize.cx <= 0 || modelSize.cy <= 0 ||
        picture.right <= picture.left || picture.bottom <= picture.top)
        return false;

    if (m_generation != source.Generation() || !EqualRect(&picture, &m_picture)) {
        Reset();
        m_generation = source.Generation();
        m_picture = picture;
    }
    if (m_estimatePts > pts || m_estimatePts < source.SceneStart(pts) ||
        (inverted && m_estimatePts >= 0 &&
        pts - m_estimatePts > 2 * CZipDepth::MaxEstimateAge)) {
        Reset();
    }
    const auto estimate = source.ForFrame(pts);
    const bool hasHistory = m_estimatePts >= 0;
    const bool hasNew = estimate.view && estimate.pts > m_estimatePts;
    if (!hasHistory && !hasNew) return false;

    D3D11_TEXTURE2D_DESC desc{};
    output->GetDesc(&desc);
    if (m_size.cx != LONG(desc.Width) || m_size.cy != LONG(desc.Height)) {
        Reset();
        if (!hasNew) return false;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, m_history.ReleaseAndGetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(m_history.Get(), nullptr, m_historyView.ReleaseAndGetAddressOf()))) {
            m_failed = true;
            return false;
        }
        m_size = {LONG(desc.Width), LONG(desc.Height)};
    }

    const auto scale = ContentScale(modelSize);
    const float age = hasNew && frameTimeMs > 0.f ? float(pts - estimate.pts) / (10000.f * frameTimeMs) : 0.f;
    const float data[12]{float(picture.left), float(picture.top), float(picture.right - picture.left),
        float(picture.bottom - picture.top), scale[0], scale[1], hasNew ? 1.f : 0.f,
        m_estimatePts >= 0 ? 1.f : 0.f, float(desc.Width), float(desc.Height), age, inverted ? 1.f : 0.f};
    auto* constants = source.m_constants.Get();
    auto* sampler = source.m_sampler.Get();
    ID3D11ShaderResourceView* inputs[]{estimate.view, m_historyView.Get(), motion};
    context->UpdateSubresource(constants, 0, nullptr, data, 0, 0);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetShaderResources(0, 3, inputs);
    context->PSSetShader(source.m_temporalShader.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* empty[3]{};
    context->PSSetShaderResources(0, 3, empty);
    context->CopyResource(m_history.Get(), output);
    if (hasNew) m_estimatePts = estimate.pts;
    return true;
}
