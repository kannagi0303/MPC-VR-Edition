// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "DecodedFrameDifference.h"
#include "Times.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <emmintrin.h>
#include <new>

namespace mfg {
namespace {

bool IsRgb(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

constexpr char CompareShader[] = R"(
#ifdef RGB_INPUT
Texture2D<float4> previousRGB : register(t0);
Texture2D<float4> currentRGB : register(t2);
#else
Texture2D<float> previousY : register(t0);
Texture2D<float2> previousUV : register(t1);
Texture2D<float> currentY : register(t2);
Texture2D<float2> currentUV : register(t3);
#endif
RWStructuredBuffer<uint2> tilesWrite : register(u0);
RWStructuredBuffer<uint> result : register(u1);
StructuredBuffer<uint2> tilesRead : register(t4);
cbuffer Visible : register(b0) { uint2 origin; uint2 extent; }

groupshared uint4 measures[64];
groupshared uint2 windowMax[64];

[numthreads(8, 8, 1)]
void Compare(uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    // Each group produces four 8x8 tiles. Each thread owns a 2x2 luma block.
    uint2 tile = group.xy * 2 + uint2((index >> 4) & 1, index >> 5);
    uint2 local = tile * 8 + uint2(index & 3, (index >> 2) & 3) * 2;
    uint4 metrics = 0;
    if (all(local < extent)) {
        uint2 p = local + origin;
#ifndef RGB_INPUT
        uint2 chroma = p / 2;
        float2 uv = abs(currentUV.Load(int3(chroma, 0)) - previousUV.Load(int3(chroma, 0)));
        float uvDelta = max(uv.x, uv.y);
#endif
        [unroll] for (uint y = 0; y < 2; ++y) {
            [unroll] for (uint x = 0; x < 2; ++x) {
                uint2 offset = uint2(x, y);
                if (all(local + offset < extent)) {
                    uint2 pixel = p + offset;
#ifdef RGB_INPUT
                    float3 diff = abs(currentRGB.Load(int3(pixel, 0)).rgb - previousRGB.Load(int3(pixel, 0)).rgb);
                    uint delta = (uint)round(255.0 * max(max(diff.r, diff.g), diff.b));
#else
                    // For an odd crop origin some pixels straddle a chroma block.
                    uint2 pixelChroma = pixel / 2;
                    float color = uvDelta;
                    if (any(pixelChroma != chroma)) {
                        float2 diff = abs(currentUV.Load(int3(pixelChroma, 0)) -
                                          previousUV.Load(int3(pixelChroma, 0)));
                        color = max(diff.x, diff.y);
                    }
                    float luma = abs(currentY.Load(int3(pixel, 0)) - previousY.Load(int3(pixel, 0)));
                    uint delta = (uint)round(255.0 * max(luma, color));
#endif
                    metrics += uint4(delta, delta > 8, delta > 16, delta > 32);
                }
            }
        }
    }
    measures[index] = metrics;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 8; stride > 0; stride /= 2) {
        if ((index & 15) < stride) measures[index] += measures[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    uint2 tileSize = (extent + 7) / 8;
    if ((index & 15) == 0 && all(tile < tileSize)) {
        // Even a full 16x16 window has a sum of at most 65,280.
        // Each counter therefore fits in 16 bits when four tiles are added.
        uint4 totals = measures[index];
        tilesWrite[tile.y * tileSize.x + tile.x] = totals.xz | (totals.yw << 16);
    }
}

[numthreads(8, 8, 1)]
void Reduce(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    uint2 tileSize = (extent + 7) / 8;
    uint4 metrics = 0;
    uint2 window = 0;
    if (all(id.xy < tileSize)) {
        uint offset = id.y * tileSize.x + id.x;
        uint2 packed = tilesRead[offset];
        metrics = uint4(packed.x & 0xffff, packed.x >> 16, packed.y & 0xffff, packed.y >> 16);
        // A 16x16 window ending at this tile reuses four 8x8 totals.
        // The frame totals count only this tile, so overlaps never count twice.
        window = packed;
        if (id.x > 0) window += tilesRead[offset - 1];
        if (id.y > 0) {
            window += tilesRead[offset - tileSize.x];
            if (id.x > 0) window += tilesRead[offset - tileSize.x - 1];
        }
        window &= 0xffff;
    }
    measures[index] = metrics;
    windowMax[index] = window;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride /= 2) {
        if (index < stride) {
            measures[index] += measures[index + stride];
            windowMax[index] = max(windowMax[index], windowMax[index + stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        uint4 totals = measures[0];
        if (totals.x) InterlockedAdd(result[0], totals.x);
        if (totals.y) InterlockedAdd(result[1], totals.y);
        if (totals.z) InterlockedAdd(result[2], totals.z);
        if (totals.w) InterlockedAdd(result[3], totals.w);
        if (windowMax[0].x) InterlockedMax(result[4], windowMax[0].x);
        if (windowMax[0].y) InterlockedMax(result[5], windowMax[0].y);
    }
}
)";

struct DifferenceMetrics {
    uint64_t sum = 0, over8 = 0, over16 = 0;

    bool WithinRepeatThreshold(uint64_t pixels) const {
        return sum * 100 <= pixels * 28 && over8 * 10000 <= pixels * 6 &&
            over16 * 100000 <= pixels * 8;
    }

    bool WithinLowMotionThreshold(uint64_t pixels) const {
        return sum * 100 <= pixels * 65 && over8 * 1000 <= pixels * 5 &&
            over16 * 1000 <= pixels;
    }
};

constexpr UINT MaxRepeatTileSum = 8 * 16 * 16;
// A small region with many strong changes can be lost in the frame average.
constexpr UINT MaxRepeatTileOver16 = 24;
constexpr UINT MaxLowMotionTileSum = 12 * 16 * 16;

FrameDifference ClassifyDifference(const DifferenceMetrics& metrics, uint64_t pixels,
                                   unsigned largestTileSum, unsigned largestTileOver16, bool measureLowMotion)
{
    if (largestTileSum <= MaxRepeatTileSum && largestTileOver16 <= MaxRepeatTileOver16 &&
        metrics.WithinRepeatThreshold(pixels))
        return FrameDifference::Duplicate;
    if (measureLowMotion && largestTileSum <= MaxLowMotionTileSum && metrics.WithinLowMotionThreshold(pixels))
        return FrameDifference::LowMotion;
    return FrameDifference::Different;
}

bool ContinuousSample(int64_t previous, int64_t current)
{
    return previous >= 0 && current > previous && current - previous <= 2'500'000;
}

unsigned PixelDifference(const BYTE* a, const BYTE* b, const BYTE* uvA, const BYTE* uvB, size_t x)
{
    const size_t chroma = x & ~size_t(1);
    const unsigned luma = std::abs(int(a[x]) - b[x]);
    const unsigned u = std::abs(int(uvA[chroma]) - uvB[chroma]);
    const unsigned v = std::abs(int(uvA[chroma + 1]) - uvB[chroma + 1]);
    return std::max({luma, u, v});
}

__m128i ByteDifferences(const BYTE* a, const BYTE* b)
{
    const auto va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
    const auto vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
    return _mm_or_si128(_mm_subs_epu8(va, vb), _mm_subs_epu8(vb, va));
}

__m128i OverThreshold(__m128i delta, char threshold)
{
    const auto excess = _mm_subs_epu8(delta, _mm_set1_epi8(threshold));
    return _mm_andnot_si128(_mm_cmpeq_epi8(excess, _mm_setzero_si128()), _mm_set1_epi8(1));
}

struct LocalDifference {
    unsigned sum = 0;
    unsigned over16 = 0;

    LocalDifference operator+(LocalDifference other) const {
        return {sum + other.sum, over16 + other.over16};
    }
};

FrameDifference CompareCpuNv12(const BYTE* previous, const BYTE* current, size_t pitch,
                              size_t height, const RECT& visible, bool measureLowMotion)
{
    const auto* previousChroma = previous + pitch * height;
    const auto* currentChroma = current + pitch * height;
    const auto pixels = uint64_t(visible.right - visible.left) * (visible.bottom - visible.top);
    // Detect hard cuts before returning early on a local difference.
    unsigned sampled = 0, strong = 0;
    for (LONG y = visible.top; y < visible.bottom; y += 16) {
        for (LONG x = visible.left; x < visible.right; x += 16) {
            strong += PixelDifference(previous + y * pitch, current + y * pitch,
                previousChroma + (y / 2) * pitch, currentChroma + (y / 2) * pitch, x) > 32;
            ++sampled;
        }
    }
    if (strong * 100u > sampled * 60u) return FrameDifference::Cut;

    DifferenceMetrics metrics;
    unsigned largestTileSum = 0;
    unsigned largestTileOver16 = 0;
    const auto zero = _mm_setzero_si128();
    std::array<LocalDifference, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 8> previousRow{};
    // Keep one row of tile totals so each pixel is compared only once.
    for (LONG top = visible.top; top < visible.bottom; top += 8) {
        LocalDifference leftTile, aboveLeft;
        const LONG bottom = std::min(top + 8, visible.bottom);
        for (LONG left = visible.left; left < visible.right; left += 16) {
            LocalDifference tiles[2]{};
            const LONG right = std::min(left + 16, visible.right);
            const bool useSimd = right - left == 16 && !(left & 1);
            auto sums = zero;
            auto over8 = zero;
            auto over16 = zero;
            for (LONG y = top; y < bottom; ++y) {
                const auto* a = previous + y * pitch;
                const auto* b = current + y * pitch;
                const auto* uvA = previousChroma + (y / 2) * pitch;
                const auto* uvB = currentChroma + (y / 2) * pitch;
                if (useSimd) {
                    // NV12 stores one U/V pair for every two luma samples.
                    const auto uv = ByteDifferences(uvA + left, uvB + left);
                    const auto uvMax = _mm_and_si128(_mm_max_epu8(uv, _mm_srli_epi16(uv, 8)), _mm_set1_epi16(0xff));
                    const auto delta = _mm_max_epu8(ByteDifferences(a + left, b + left),
                        _mm_or_si128(uvMax, _mm_slli_epi16(uvMax, 8)));
                    sums = _mm_add_epi64(sums, _mm_sad_epu8(delta, zero));
                    over8 = _mm_add_epi8(over8, OverThreshold(delta, 8));
                    over16 = _mm_add_epi8(over16, OverThreshold(delta, 16));
                    continue;
                }
                for (LONG x = left; x < right; ++x) {
                    const auto delta = PixelDifference(a, b, uvA, uvB, x);
                    auto& tile = tiles[(x - left) / 8];
                    tile.sum += delta;
                    metrics.over8 += delta > 8;
                    tile.over16 += delta > 16;
                }
            }
            if (useSimd) {
                const auto count8 = _mm_sad_epu8(over8, zero);
                const auto count16 = _mm_sad_epu8(over16, zero);
                metrics.over8 += unsigned(_mm_cvtsi128_si32(count8) + _mm_cvtsi128_si32(_mm_srli_si128(count8, 8)));
                tiles[0] = {unsigned(_mm_cvtsi128_si32(sums)), unsigned(_mm_cvtsi128_si32(count16))};
                tiles[1] = {unsigned(_mm_cvtsi128_si32(_mm_srli_si128(sums, 8))),
                            unsigned(_mm_cvtsi128_si32(_mm_srli_si128(count16, 8)))};
            }
            for (LONG i = 0; i * 8 < right - left; ++i) {
                auto& above = previousRow[(left - visible.left) / 8 + i];
                const auto& tile = tiles[i];
                const auto window = tile + leftTile + above + aboveLeft;
                metrics.sum += tile.sum;
                metrics.over16 += tile.over16;
                largestTileSum = std::max(largestTileSum, window.sum);
                largestTileOver16 = std::max(largestTileOver16, window.over16);
                aboveLeft = above;
                above = tile;
                leftTile = tile;
            }
            if (ClassifyDifference(metrics, pixels, largestTileSum, largestTileOver16, measureLowMotion) == FrameDifference::Different)
                return FrameDifference::Different;
        }
    }
    return ClassifyDifference(metrics, pixels, largestTileSum, largestTileOver16, measureLowMotion);
}

} // namespace

bool DecodedFrameDifference::Initialize(ID3D11Device1* device, const D3D11_TEXTURE2D_DESC& source)
{
    const auto level = device->GetFeatureLevel();
    D3D_FEATURE_LEVEL chosen{};
    if (FAILED(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device1), &chosen, &state_))) {
        return false;
    }
    const bool rgb = IsRgb(source.Format);
    const D3D_SHADER_MACRO macros[] = {{"RGB_INPUT", "1"}, {nullptr, nullptr}};
    const auto compile = [&](const char* entry, ID3D11ComputeShader** shader) {
        ComPtr<ID3DBlob> code;
        return SUCCEEDED(D3DCompile(CompareShader, sizeof(CompareShader) - 1, nullptr, rgb ? macros : nullptr, nullptr,
            entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) &&
            SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader));
    };
    if (!compile("Compare", compare_.GetAddressOf()) || !compile("Reduce", reduce_.GetAddressOf())) {
        return false;
    }

    D3D11_TEXTURE2D_DESC picture{};
    picture.Width = source.Width;
    picture.Height = source.Height;
    picture.MipLevels = picture.ArraySize = 1;
    picture.Format = source.Format;
    picture.SampleDesc.Count = 1;
    picture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    for (auto* slot : {&previous_, &current_}) {
        if (FAILED(device->CreateTexture2D(&picture, nullptr, &slot->texture))) return false;
        view.Format = rgb ? source.Format : format_ == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
        if (FAILED(device->CreateShaderResourceView(slot->texture.Get(), &view, &slot->luma))) return false;
        if (!rgb) {
            view.Format = format_ == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
            if (FAILED(device->CreateShaderResourceView(slot->texture.Get(), &view, &slot->chroma))) return false;
        }
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 4 * sizeof(UINT);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &area_))) return false;

    const UINT tileCount = ((source.Width + 7) / 8) * ((source.Height + 7) / 8);
    buffer.StructureByteStride = 2 * sizeof(UINT);
    buffer.ByteWidth = tileCount * buffer.StructureByteStride;
    buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = tileCount;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv.Buffer.NumElements = tileCount;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &tiles_)) ||
        FAILED(device->CreateUnorderedAccessView(tiles_.Get(), &uav, &tilesWrite_)) ||
        FAILED(device->CreateShaderResourceView(tiles_.Get(), &srv, &tilesRead_))) {
        return false;
    }

    buffer.ByteWidth = 6 * sizeof(UINT);
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer.StructureByteStride = sizeof(UINT);
    uav.Buffer.NumElements = 6;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &result_)) ||
        FAILED(device->CreateUnorderedAccessView(result_.Get(), &uav, &resultView_))) {
        return false;
    }
    buffer.Usage = D3D11_USAGE_STAGING;
    buffer.BindFlags = buffer.MiscFlags = buffer.StructureByteStride = 0;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &readback_));
}

void DecodedFrameDifference::Submit(ID3D11Device1* device, ID3D11DeviceContext1* context,
    ID3D11Texture2D* source, UINT slice, const RECT& visible, int64_t pts)
{
    cpuPreviousPts_ = cpuPendingPts_ = -1;
    cpuAnchor_ = false;
    pending_ = false;
    if (!source || pts < 0) {
        Reset();
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if ((!IsRgb(desc.Format) && desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) ||
        desc.SampleDesc.Count != 1 || slice >= desc.ArraySize || desc.MipLevels != 1 ||
        visible.left < 0 || visible.top < 0 || visible.right <= visible.left ||
        visible.bottom <= visible.top || UINT(visible.right) > desc.Width || UINT(visible.bottom) > desc.Height) {
        Reset();
        return;
    }
    if (device_.Get() != device || width_ != desc.Width || height_ != desc.Height || format_ != desc.Format) {
        *this = {};
        device_ = device;
        width_ = desc.Width;
        height_ = desc.Height;
        format_ = desc.Format;
        ready_ = Initialize(device, desc);
        ComPtr<ID3D11Device5> fenceDevice;
        if (ready_ && SUCCEEDED(device_.As(&fenceDevice)) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&fenceContext_)))) {
            fenceDevice->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        }
    }
    if (!ready_) return;

    pendingPts_ = pts;
    context->CopySubresourceRegion(current_.texture.Get(), 0, 0, 0, 0, source, slice, nullptr);
    if (gpuAnchor_ && ContinuousSample(previousPts_, pts)) {
        const UINT rect[] = {UINT(visible.left), UINT(visible.top),
            UINT(visible.right - visible.left), UINT(visible.bottom - visible.top)};
        pixels_ = uint64_t(rect[2]) * rect[3];
        ComPtr<ID3DDeviceContextState> saved;
        context->SwapDeviceContextState(state_.Get(), &saved);
        context->UpdateSubresource(area_.Get(), 0, nullptr, rect, 0, 0);
        const UINT zero[4]{};
        context->ClearUnorderedAccessViewUint(resultView_.Get(), zero);
        ID3D11ShaderResourceView* inputs[] = {previous_.luma.Get(), previous_.chroma.Get(),
                                             current_.luma.Get(), current_.chroma.Get(), nullptr};
        ID3D11Buffer* area = area_.Get();
        ID3D11UnorderedAccessView* outputs[] = {tilesWrite_.Get(), nullptr};
        context->CSSetShader(compare_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &area);
        context->CSSetShaderResources(0, 5, inputs);
        context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
        context->Dispatch((rect[2] + 15) / 16, (rect[3] + 15) / 16, 1);

        outputs[0] = nullptr;
        outputs[1] = resultView_.Get();
        context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
        ID3D11ShaderResourceView* tiles = tilesRead_.Get();
        context->CSSetShaderResources(4, 1, &tiles);
        context->CSSetShader(reduce_.Get(), nullptr, 0);
        context->Dispatch((rect[2] + 63) / 64, (rect[3] + 63) / 64, 1);

        ID3D11ShaderResourceView* empty[5]{};
        ID3D11UnorderedAccessView* noOutputs[2]{};
        context->CSSetShaderResources(0, 5, empty);
        context->CSSetUnorderedAccessViews(0, 2, noOutputs, nullptr);
        context->CopyResource(readback_.Get(), result_.Get());
        if (fence_ && FAILED(fenceContext_->Signal(fence_.Get(), ++fenceValue_))) fence_.Reset();
        context->SwapDeviceContextState(saved.Get(), nullptr);
        pending_ = true;
        // Let the readback overlap the rest of frame processing.
        context->Flush();
    }
    previousPts_ = pts;
}

void DecodedFrameDifference::SubmitCpuNv12(const BYTE* source, size_t pitch, size_t height,
    const RECT& visible, int64_t pts, bool measureLowMotion)
{
    previousPts_ = pendingPts_ = -1;
    pending_ = gpuAnchor_ = false;
    cpuPendingPts_ = pts;
    cpuResult_ = FrameDifference::Unknown;
    if (!source || pts < 0 || !pitch || pitch % 2 || !height || height % 2 ||
        visible.left < 0 || visible.top < 0 || visible.right <= visible.left ||
        visible.bottom <= visible.top || size_t(visible.right) > pitch || size_t(visible.bottom) > height ||
        visible.right - visible.left > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        Reset();
        return;
    }
    const size_t bytes = pitch * (height + height / 2);
    if (cpuPitch_ != pitch || cpuHeight_ != height || previousCpu_.size() != bytes || currentCpu_.size() != bytes) {
        cpuAnchor_ = false;
        try {
            previousCpu_.resize(bytes);
            currentCpu_.resize(bytes);
        } catch (const std::bad_alloc&) {
            Reset();
            return;
        }
        cpuPitch_ = pitch;
        cpuHeight_ = height;
    }
    if (cpuAnchor_ && ContinuousSample(cpuPreviousPts_, pts)) {
        cpuResult_ = CompareCpuNv12(previousCpu_.data(), source, pitch, height, visible, measureLowMotion);
    }
    std::memcpy(currentCpu_.data(), source, bytes);
    cpuPreviousPts_ = pts;
}

void DecodedFrameDifference::KeepCurrent(int64_t pts)
{
    if (pts < 0) return;
    if (cpuPendingPts_ == pts) {
        previousCpu_.swap(currentCpu_);
        cpuAnchor_ = true;
        cpuPendingPts_ = -1;
    } else if (pendingPts_ == pts) {
        std::swap(previous_, current_);
        gpuAnchor_ = true;
        pendingPts_ = -1;
    } else {
        Reset();
    }
}

FrameDifference DecodedFrameDifference::Take(ID3D11DeviceContext1* context, int64_t pts, bool measureLowMotion)
{
    readbackResult_ = S_FALSE;
    if (cpuPendingPts_ == pts && pts >= 0) {
        if (cpuResult_ != FrameDifference::Unknown) readbackResult_ = S_OK;
        return cpuResult_;
    }
    if (!pending_ || pendingPts_ != pts) return FrameDifference::Unknown;
    pending_ = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto read = [&]() -> HRESULT {
        if (fence_ && fence_->GetCompletedValue() < fenceValue_) return DXGI_ERROR_WAS_STILL_DRAWING;
        return context->Map(readback_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    };
    auto hr = read();
    // On a persistently busy queue, still check every sample but spend the
    // bounded retry budget only periodically. A ready result restores retries.
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && consecutiveBusy_ % 4 == 0) {
        // Video conversion has already run after Submit. Give any remaining
        // work a short opportunity to finish, then keep the original sample.
        const auto start = GetPreciseTick();
        const auto budget = GetPreciseTicksPerSecondI() / 500; // 2 ms
        if (!fence_) context->Flush();
        do {
            hr = read();
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
            SwitchToThread();
        } while (GetPreciseTick() - start < budget);
    }
    consecutiveBusy_ = hr == DXGI_ERROR_WAS_STILL_DRAWING ? consecutiveBusy_ + 1 : 0;
    readbackResult_ = hr;
    if (FAILED(hr)) return FrameDifference::Unknown;
    const auto* result = static_cast<const UINT*>(mapped.pData);
    const DifferenceMetrics metrics{result[0], result[1], result[2]};
    const uint64_t strong = result[3];
    const auto largestTileSum = result[4];
    const auto largestTileOver16 = result[5];
    context->Unmap(readback_.Get(), 0);
    if (strong * 100 > pixels_ * 60) return FrameDifference::Cut;
    return ClassifyDifference(metrics, pixels_, largestTileSum, largestTileOver16, measureLowMotion);
}

} // namespace mfg
