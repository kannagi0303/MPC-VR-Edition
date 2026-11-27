// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Depth.h"
#include "DepthDrawShader.h"
#include "DepthLookahead.h"
#include "DepthRawPolicy.h"
#include "DLSS/NgxRuntime.h"
#include "Mfg/MotionShaders.h"
#include "Edition/FixedShaderBytecode.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <mutex>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace {
class DepthTrace {
    FILE* m_file = nullptr;
    std::mutex m_lock;
public:
    DepthTrace() {
        wchar_t path[1024]{};
        DWORD length = GetEnvironmentVariableW(L"MPCVR_DEPTH_TRACE", path, _countof(path));
        if (!length && depthsettings::LookaheadMs()) {
            length = GetPrivateProfileStringW(L"Depth", L"Trace", L"", path, _countof(path), depthsettings::LookaheadConfig().c_str());
        }
        if (!length || length >= _countof(path) || _wfopen_s(&m_file, path, L"wb") || !m_file) return;
        std::fputs("event,pts_100ns,estimate_pts_100ns,media_age_ms,processing_ms,gpu_pack_ms,gpu_model_ms,gpu_post_ms,cut,ready_ahead_ms\n",
            m_file);
    }
    ~DepthTrace() { if (m_file) std::fclose(m_file); }
    bool Enabled() const { return m_file != nullptr; }
    void Record(const char* event, int64_t pts, int64_t estimatePts, double processingMs = 0,
        CDepthGpu::Timings timings = {}, bool cut = false, int64_t ahead = 0) {
        if (!m_file) return;
        std::lock_guard guard(m_lock);
        std::fprintf(m_file, "%s,%lld,%lld,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f\n", event,
            (long long)pts, (long long)estimatePts, estimatePts >= 0 ? double(pts - estimatePts) / 10000.0 : -1.0, processingMs,
            timings.available ? timings.packMs : -1.0, timings.available ? timings.modelMs : -1.0,
            timings.available ? timings.postMs : -1.0, unsigned(cut), double(ahead) / 10000.0);
    }
};

DepthTrace& TraceDepth() {
    static DepthTrace trace;
    return trace;
}

constexpr char CaptureShader[] = R"(
Texture2D<float4> Source : register(t0);
Texture2D<float2> Chroma : register(t1);
SamplerState Linear : register(s0);
cbuffer Settings : register(b0) {
    float4 Picture; // normalized origin and extent in the source texture
    float4 Model;   // tensor width, height, clockwise rotation, full range NV12
    float4 Padding;
};
float2 SourceUV(float4 pos) {
    float2 uv = pos.xy / Model.xy;
    if (Model.z > 0.5)
        uv = float2(uv.y, 1.0 - uv.x);
    uint w, h;
    Source.GetDimensions(w, h);
    float2 halfPixel = 0.5 / float2(w, h);
    return clamp(Picture.xy + uv * Picture.zw,
        Picture.xy + halfPixel, Picture.xy + Picture.zw - halfPixel);
}
float4 Main(float4 pos : SV_POSITION) : SV_TARGET {
    return float4(saturate(Source.SampleLevel(Linear, SourceUV(pos), 0).rgb), 1.0);
}
float4 Nv12(float4 pos : SV_POSITION) : SV_TARGET {
    const float2 uv = SourceUV(pos);
    float y = Source.SampleLevel(Linear, uv, 0).r;
    float2 c = Chroma.SampleLevel(Linear, uv, 0) - 128.0 / 255.0;
    if (Model.w < 0.5) {
        y = (y * 255.0 - 16.0) / 219.0;
        c *= 255.0 / 224.0;
    }
    return float4(saturate(y + float3(1.5748*c.y, -0.187324*c.x-0.468124*c.y, 1.8556*c.x)), 1.0);
}
)";

template<auto& Source, edition::FixedShaderString Entry>
bool CreatePixelShader(ID3D11Device1* device, ID3D11PixelShader** shader)
{
    ComPtr<ID3DBlob> code;
    return SUCCEEDED((edition::CompileFixedShader<Source,Entry,"ps_5_0">(code.GetAddressOf()))) &&
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
    buffer.ByteWidth = 80;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateSamplerState(&desc, sampler)) &&
        SUCCEEDED(device->CreateBuffer(&buffer, nullptr, constants));
}

bool RotateModelInput(SIZE picture, SIZE tensor)
{
    return tensor.cx > tensor.cy && picture.cy > picture.cx;
}
}

bool CDepth::Initialize(ID3D11Device1* device)
{
    m_device = device;
    const auto featureLevel = device->GetFeatureLevel();
    D3D_FEATURE_LEVEL chosen{};
    ComPtr<ID3DBlob> vertex;
    if (FAILED(device->CreateDeviceContextState(0, &featureLevel, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device1), &chosen, m_state.GetAddressOf())) ||
        FAILED((edition::CompileFixedShader<mfg::MotionShaders,"VS","vs_5_0">(vertex.GetAddressOf()))) ||
        FAILED(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
            nullptr, m_vertex.GetAddressOf())) ||
        !CreatePixelShader<CaptureShader,"Main">(device, m_captureShader.GetAddressOf()) ||
        !CreatePixelShader<CaptureShader,"Nv12">(device, m_captureNv12Shader.GetAddressOf()) ||
        !CreatePixelShader<DepthDrawShader,"RawExpand">(device, m_depthShader.GetAddressOf()) ||
        !CreateInputs(device, m_sampler.GetAddressOf(), m_constants.GetAddressOf()))
        return false;

    const auto directory = std::filesystem::path(NgxRuntime::EditionDir());
    return m_gpu.Start(device, directory);
}

void CDepth::Prepare(ID3D11Device1* device)
{
    if (m_device.Get() == device) return;
    if (m_device) Stop();
    m_initialized = Initialize(device);
}

void CDepth::RecordResultAge(int64_t age, uint64_t now)
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

void CDepth::Update(ID3D11Device1* device, ID3D11DeviceContext1* context,
    ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts)
{
    if (m_lookahead) {
        PollCompleted();
        return;
    }
    Capture(device, context, source, picture, modelSize, pts);
}

bool CDepth::PrefetchNv12(ID3D11Device1* device, ID3D11DeviceContext1* context,
    ID3D11Texture2D* frame, const RECT& picture, bool fullRange, int64_t pts)
{
    Prepare(device);
    PollCompleted();
    if (!m_initialized || m_gpu.Capacity() <= CDepthGpu::NormalSlots || !m_gpu.CanStart()) return true;
    if (!m_gpu.CanAccept()) return false;
    if (m_playbackPts >= 0 && pts < m_playbackPts) return true;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    ComPtr<ID3D11ShaderResourceView> luma, chroma;
    if (FAILED(device->CreateShaderResourceView(frame, &desc, &luma))) return true;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device->CreateShaderResourceView(frame, &desc, &chroma))) return true;
    m_lookahead = true;
    Capture(device, context, luma.Get(), picture, {picture.right - picture.left, picture.bottom - picture.top},
        pts, chroma.Get(), fullRange);
    return true;
}

void CDepth::PlaybackFrame(int64_t pts, int64_t duration)
{
    if (pts != m_playbackPts) {
        ClearOriginalSelection();
        m_previousPlaybackPts = m_playbackPts >= 0 && m_playbackPts < pts ? m_playbackPts : -1;
        m_playbackPts = pts;
    }
    if (duration > 0) m_originalDuration = duration;
    if (!m_lookahead) return;
    PollCompleted();
    if (!TraceDepth().Enabled()) return;
    auto ordered = m_resultPts;
    std::sort(ordered.begin(), ordered.end());
    auto ready = pts;
    const auto estimatePts = EstimateAtOrBefore(pts).pts;
    auto last = estimatePts;
    if (last >= SceneStart(pts) && pts - last <= std::max<int64_t>(duration, 1)) {
        for (const auto future : ordered) {
            if (future <= pts) continue;
            if (future - last > std::max<int64_t>(duration + duration / 2, 1)) break;
            ready = last = future;
        }
    }
    TraceDepth().Record("lookahead_ready", pts, estimatePts, 0, {}, false, ready - pts);
}

void CDepth::Capture(ID3D11Device1* device, ID3D11DeviceContext1* context,
    ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts,
    ID3D11ShaderResourceView* chroma, bool fullRange)
{
    if (!source || pts < 0 || modelSize.cx <= 0 || modelSize.cy <= 0)
        return;
    Prepare(device);
    if (!m_initialized) return;
    // Seeing a PTS does not mean its GPU job was accepted. A later existing
    // raw-source call site may retry a missed submission, without a new wait
    // or retry loop. After selection, keep the OLD admission rule: only the
    // original first-capture path or a confirmed cut can submit a late map.
    // It may help the next original, never replace this one's fixed choice.
    if (pts == m_lastSourcePts && (pts == m_lastSubmittedPts ||
        (pts == m_selectedSourcePts && pts != m_sceneCuts.back()))) return;
    const auto resetCaptureHistory = [&] {
        const bool selected = pts == m_selectedSourcePts;
        ResetHistory();
        // Geometry invalidation is stronger than a pin. Keep a neutral
        // decision for this turn, not a map from the previous geometry.
        if (selected) m_selectedSourcePts = pts;
    };
    // One successful capture per source PTS across the SR and MFG call sites.
    if (m_modelSize.cx != modelSize.cx || m_modelSize.cy != modelSize.cy ||
        pts < m_lastSourcePts || (m_lastSourcePts >= 0 && pts - m_lastSourcePts > 2'500'000)) {
        resetCaptureHistory();
        m_modelSize = modelSize;
    }
    m_lastSourcePts = pts;

    PollCompleted();
    if ((!m_lookahead && CoolingDown(GetTickCount64())) || !m_gpu.CanAccept()) return;

    ComPtr<ID3D11Resource> resource;
    source->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (picture.left < 0 || picture.top < 0 || picture.right > LONG(desc.Width) ||
        picture.bottom > LONG(desc.Height) || picture.right <= picture.left || picture.bottom <= picture.top)
        return;

    const SIZE captureSize{LONG(desc.Width), LONG(desc.Height)};
    if (m_captureSize.cx && (m_captureSize.cx != captureSize.cx || m_captureSize.cy != captureSize.cy ||
        !EqualRect(&picture, &m_capturePicture))) {
        resetCaptureHistory();
        m_lastSourcePts = pts;
    }
    m_captureSize = captureSize;
    m_capturePicture = picture;
    // A pending/running/unharvested output owns its slot even though its PTS
    // is temporarily -1. Select before drawing; the worker cannot acquire
    // a different output without this serialized producer submitting it.
    unsigned slot = m_gpu.Capacity();
    for (unsigned i = 0; i < m_gpu.Capacity(); ++i) {
        // Until the next original, preserve the selected map's CONTENT as
        // well as its identity. No extra textures or worker-side pin locks.
        if (m_selectedDepthPts >= 0 && m_resultPts[i] == m_selectedDepthPts) continue;
        if ((slot == m_gpu.Capacity() || m_resultPts[i] < m_resultPts[slot]) && !m_gpu.SlotBusy(i)) slot = i;
    }
    if (slot == m_gpu.Capacity() || (m_lookahead && m_resultPts[slot] > m_playbackPts)) return;
    unsigned captureBank = 0;
    auto* target = m_gpu.Target(captureBank);
    if (!target) return;
    const auto tensor = TensorSize();
    const float data[20]{float(picture.left) / desc.Width, float(picture.top) / desc.Height,
        float(picture.right - picture.left) / desc.Width, float(picture.bottom - picture.top) / desc.Height,
        float(tensor.cx), float(tensor.cy), RotateModelInput(modelSize, tensor) ? 1.f : 0.f, fullRange ? 1.f : 0.f};
    const D3D11_VIEWPORT viewport{0, 0, float(tensor.cx), float(tensor.cy), 0, 1};
    ComPtr<ID3DDeviceContextState> previous;
    context->SwapDeviceContextState(m_state.Get(), previous.GetAddressOf());
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertex.Get(), nullptr, 0);
    context->PSSetShader(chroma ? m_captureNv12Shader.Get() : m_captureShader.Get(), nullptr, 0);
    auto* constants = m_constants.Get();
    auto* sampler = m_sampler.Get();
    context->UpdateSubresource(constants, 0, nullptr, data, 0, 0);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* inputs[]{source, chroma};
    context->PSSetShaderResources(0, 2, inputs);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* empty[2]{};
    context->PSSetShaderResources(0, 2, empty);
    context->SwapDeviceContextState(previous.Get(), nullptr);
    if (m_gpu.Submit(context, {pts, m_generation, slot}, captureBank)) {
        // Stop advertising this slot before the GPU can overwrite it. The
        // D3D11 fence also retires its earlier uses on this immediate context.
        m_resultPts[slot] = -1;
        m_lastSubmittedPts = pts;
        ++m_counters.captures;
        ++m_counters.jobs;
    }
}

void CDepth::PollCompleted()
{
    if (!m_initialized) return;
    const auto now = GetTickCount64();
    CDepthGpu::Result estimate;
    while (m_gpu.Take(estimate)) {
        if (estimate.generation != m_generation || estimate.pts > m_lastSourcePts) continue;
        if (estimate.sceneCut) MarkSceneCut(estimate.pts);
        if (estimate.valid) {
            m_counters.inferenceMs = estimate.inferenceMs;
            m_counters.gpuTimings = estimate.timings;
            m_resultPts[estimate.slot] = estimate.pts;
            ++m_counters.maps;
            const auto clockPts = m_lookahead && m_playbackPts >= 0 ? m_playbackPts : m_lastSourcePts;
            TraceDepth().Record("estimate", clockPts, estimate.pts, estimate.inferenceMs,
                estimate.timings, estimate.sceneCut);
            if (estimate.pts >= SceneStart(clockPts))
                RecordResultAge(std::max<int64_t>(0, clockPts - estimate.pts), now);
        }
    }
    // Wall-clock rate of valid maps actually published, not source FPS or
    // the reciprocal of a single GPU inference. Do not average an idle gap
    // (e.g. pause) into the first active sample after it.
    if (!m_mapRateTick || now - m_mapRateLastPoll > 2000) {
        m_mapRateTick = now;
        m_mapRateBase = m_counters.maps;
        m_counters.mapRateSampleTick = 0;
    } else if (now - m_mapRateTick >= 1000) {
        m_counters.mapsPerSecond = double(m_counters.maps - m_mapRateBase) * 1000.0 / double(now - m_mapRateTick);
        m_counters.mapRateSampleTick = now;
        m_mapRateTick = now;
        m_mapRateBase = m_counters.maps;
    }
    m_mapRateLastPoll = now;
}

void CDepth::AwaitOriginal(int64_t pts, int64_t duration, HANDLE cancelEvent)
{
    if (pts < 0 || !m_initialized) return;
    if (duration > 0) m_originalDuration = duration;
    PollCompleted();
    // An original's first decision (including neutral) is final for this
    // producer turn. Never switch to a later completion or wait twice.
    if (m_selectedSourcePts == pts || m_waitedOriginalPts == pts) return;
    // No artificial delay if this original has not been submitted, the model
    // is loading, or its exact result is already available.
    const auto exactReady = [&] {
        const auto e = EstimateAtOrBefore(pts);
        return e.view && e.pts == pts && e.pts >= SceneStart(pts);
    };
    const bool submitted = m_lastSubmittedPts == pts ||
        (m_lookahead && m_lastSubmittedPts >= pts);
    if (submitted || exactReady()) m_waitedOriginalPts = pts;
    const auto started = std::chrono::steady_clock::now();
    const auto budget = std::chrono::microseconds((std::max<int64_t>)(0,
        (std::min<int64_t>)(m_originalDuration / 10, 40'000)));
    const auto deadline = started + budget;
    while (submitted && !exactReady() && m_gpu.CanStart() && !m_gpu.Loading() &&
           cancelEvent && std::chrono::steady_clock::now() < deadline) {
        // No GPU/NGX lock is held here. GPU worker owns its own queue and
        // Submit already flushed the D3D11 capture fence. Stop/seek wakes
        // this wait through the existing playback cancellation event.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || WaitForSingleObject(cancelEvent, 1) != WAIT_TIMEOUT) break;
        PollCompleted();
    }
    PollCompleted();
    if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) return;
    const auto selected = SelectOriginal(pts);
    const double waited = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    TraceDepth().Record(selected.view ? (selected.pts == pts ? "original_exact" : "original_previous") :
        "original_missing", pts, selected.view ? selected.pts : -1, waited);
}

CDepth::Estimate CDepth::SelectOriginal(int64_t pts)
{
    const auto selected = ForFrame(pts);
    // Only the actual decoded source turn is pinned. Generated and delayed
    // timestamps retain the existing consumer rules, never this pin.
    if (pts >= 0 && pts == m_playbackPts && m_selectedSourcePts != pts) {
        m_selectedSourcePts = pts;
        m_selectedDepthPts = selected.view ? selected.pts : -1;
    }
    return selected;
}

CDepth::Estimate CDepth::ForFrame(int64_t pts) const
{
    if (pts >= 0 && pts == m_selectedSourcePts) {
        if (m_selectedDepthPts < 0) return {};
        const auto selected = EstimateAtOrBefore(m_selectedDepthPts);
        // A newly confirmed cut can veto a past map, but must not silently
        // substitute a different map for an already composed original.
        return selected.view && selected.pts == m_selectedDepthPts &&
            selected.pts >= SceneStart(pts) ? selected : Estimate{};
    }
    const auto estimate = EstimateAtOrBefore(pts);
    if (!estimate.view || estimate.pts < SceneStart(pts) || pts - estimate.pts > MaxEstimateAge) return {};
    if (estimate.pts == pts) return estimate;
    // Only the immediately preceding decoded original is an eligible fallback
    // for the current source PTS. Delayed consumers use one source interval.
    if (pts == m_playbackPts && m_previousPlaybackPts >= 0)
        return estimate.pts == m_previousPlaybackPts ? estimate : Estimate{};
    return depthraw::Usable(estimate.pts, pts, float(m_originalDuration) / 10000.0f)
        ? estimate : Estimate{};
}

CDepth::Estimate CDepth::LatestPreview() const
{
    // Diagnostics show the newest completed result independently of the presenter.
    return EstimateAtOrBefore(INT64_MAX);
}

CDepth::CacheStats CDepth::GetCacheStats(int64_t playbackPts) const
{
    CacheStats stats;
    stats.capacity = m_gpu.Capacity();
    for (unsigned slot = 0; slot < stats.capacity; ++slot) {
        const auto pts = m_resultPts[slot];
        if (pts < 0) continue;
        ++stats.cached;
        if (playbackPts >= 0 && pts > playbackPts) ++stats.ahead;
    }
    return stats;
}

CDepth::Estimate CDepth::EstimateAtOrBefore(int64_t pts) const
{
    Estimate estimate;
    for (unsigned slot = 0; slot < m_resultPts.size(); ++slot) {
        const auto resultPts = m_resultPts[slot];
        if (resultPts > estimate.pts && resultPts <= pts)
            estimate = {m_gpu.View(slot), resultPts, RotateModelInput(m_modelSize, TensorSize())};
    }
    return estimate;
}

int64_t CDepth::SceneStart(int64_t pts) const
{
    const auto cut = std::upper_bound(m_sceneCuts.begin(), m_sceneCuts.end(), pts);
    return cut == m_sceneCuts.begin() ? -1 : *std::prev(cut);
}

void CDepth::MarkSceneCut(int64_t pts)
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

void CDepth::ResetHistory()
{
    ++m_generation;
    m_lastSourcePts = m_lastSubmittedPts = -1;
    m_waitedOriginalPts = -1;
    ClearOriginalSelection();
    m_previousPlaybackPts = -1;
    m_resultPts.fill(-1);
    m_sceneCuts.fill(-1);
    m_captureSize = {};
    m_mapRateTick = m_mapRateLastPoll = 0;
    m_counters.mapRateSampleTick = 0;
    m_counters.rawMfgCheckAge = -1;
    m_counters.rawMfgCheck = L"not checked";
    m_gpu.SetSceneStart(-1);
}

void CDepth::Stop(bool fastShutdown)
{
    m_gpu.Stop(fastShutdown);
    ResetHistory();
    m_device.Reset();
    m_state.Reset();
    m_vertex.Reset();
    m_captureShader.Reset();
    m_captureNv12Shader.Reset();
    m_depthShader.Reset();
    m_sampler.Reset();
    m_constants.Reset();
    m_counters = {};
    m_mapRateTick = 0;
    m_mapRateBase = 0;
    m_consecutiveLate = 0;
    m_retryAt = 0;
    m_initialized = false;
}

const wchar_t* CDepth::Status() const
{
    if (!m_device) return L"awaiting source";
    return m_gpu.Status(LatestPreview().view != nullptr);
}

bool CDepth::DrawEstimate(ID3D11DeviceContext1* context,
    ID3D11Texture2D* output, ID3D11RenderTargetView* target, const RECT& picture,
    SIZE modelSize, int64_t pts, float frameTimeMs, bool inverted,
    ID3D11ShaderResourceView* active)
{
    if (!m_initialized || pts < 0 || modelSize.cx <= 0 || modelSize.cy <= 0 ||
        picture.right <= picture.left || picture.bottom <= picture.top)
        return false;
    // Harvest again at the point of consumption: capture may have polled
    // before the preceding asynchronous job finished. No wait or GPU query.
    PollCompleted();
    // Current decoded original uses the same fixed choice as VE/Overlay.
    // Keep the original RAW consumer restrictions, including the actual
    // interval bound; delayed/generated phases retain their old lookup.
    const auto estimate = pts == m_playbackPts ? SelectOriginal(pts) : EstimateAtOrBefore(pts);
    const auto record = [&](const wchar_t* status) {
        if (inverted) { // MFG, not the SR consumer
            m_counters.rawMfgCheckAge = estimate.view ? pts - estimate.pts : -1;
            m_counters.rawMfgCheck = status;
        }
    };
    if (!estimate.view) { record(L"no map"); return false; }
    if (estimate.pts < SceneStart(pts)) { record(L"scene mismatch"); return false; }
    if (pts - estimate.pts > CDepth::MaxEstimateAge) { record(L"too old"); return false; }
    if (!m_depthShader) { record(L"shader unavailable"); return false; }
    if (!depthraw::Usable(estimate.pts, pts, frameTimeMs) ||
        (pts == m_playbackPts && estimate.pts != pts && !ForFrame(pts).view)) {
        record(L"PTS mismatch"); return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    output->GetDesc(&desc);
    const float data[20]{float(picture.left), float(picture.top), float(picture.right - picture.left),
        float(picture.bottom - picture.top), estimate.rotated ? 1.f : 0.f, inverted ? 1.f : 0.f,
        active ? 1.f : 0.f, OutputDepthSpan};
    auto* constants = m_constants.Get();
    auto* sampler = m_sampler.Get();
    ID3D11ShaderResourceView* inputs[]{estimate.view, nullptr, nullptr, nullptr, nullptr, active, nullptr};
    context->UpdateSubresource(constants, 0, nullptr, data, 0, 0);
    context->PSSetConstantBuffers(0, 1, &constants);
    context->PSSetSamplers(0, 1, &sampler);
    context->PSSetShaderResources(0, UINT(std::size(inputs)), inputs);
    context->PSSetShader(m_depthShader.Get(), nullptr, 0);
    const D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* empty[std::size(inputs)]{};
    context->PSSetShaderResources(0, UINT(std::size(empty)), empty);
    record(L"used");
    TraceDepth().Record(inverted ? "mfg_use" : "sr_use", pts, estimate.pts);
    return true;
}
