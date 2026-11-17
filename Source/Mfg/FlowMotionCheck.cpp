// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "FlowMotionCheck.h"
#include <d3dcompiler.h>

namespace mfg {
namespace {

constexpr char ReduceFlow[] = R"(
Texture2D<int2> vectors : register(t0);
Texture2D<int4> active : register(t1);
cbuffer Params : register(b0) { int grid; };
RWStructuredBuffer<uint> result : register(u0);
groupshared uint moving[64];
groupshared uint localMotion[64];
groupshared uint covered[64];

bool InContent(uint2 p, int4 bounds)
{
    const int2 origin = int2(p) * grid;
    return all(origin < bounds.zw) && all(origin + grid > bounds.xy);
}

bool MovesAtLeast(uint2 p, float limit)
{
    // NVOFA stores signed pixel displacements with 5 fractional bits.
    const float2 v = float2(vectors.Load(int3(p, 0)));
    return dot(v, v) >= limit * limit;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    uint2 size;
    vectors.GetDimensions(size.x, size.y);
    const uint2 p = id.xy;
    const int4 bounds = active.Load(int3(0, 0, 0));
    const bool inside = all(p < size) && InContent(p, bounds);
    uint moderate = 0, coherent = 0;
    if (inside) {
        moderate = MovesAtLeast(p, 32.0);
        if (MovesAtLeast(p, 128.0)) {
            const uint2 right = p + uint2(1, 0), below = p + uint2(0, 1);
            coherent = (right.x < size.x && InContent(right, bounds) && MovesAtLeast(right, 128.0)) ||
                       (below.y < size.y && InContent(below, bounds) && MovesAtLeast(below, 128.0));
        }
    }
    moving[index] = moderate;
    localMotion[index] = coherent;
    covered[index] = inside;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 32; step > 0; step /= 2) {
        if (index < step) {
            moving[index] += moving[index + step];
            localMotion[index] |= localMotion[index + step];
            covered[index] += covered[index + step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) {
        if (moving[0]) InterlockedAdd(result[0], moving[0]);
        if (localMotion[0]) InterlockedOr(result[1], 1);
        if (covered[0]) InterlockedAdd(result[2], covered[0]);
    }
}
)";

} // namespace

bool FlowMotionCheck::Initialize(ID3D11Device* device)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    if (FAILED(D3DCompile(ReduceFlow, sizeof(ReduceFlow) - 1, nullptr, nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) ||
        FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
            nullptr, &reduce_))) return false;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 3 * sizeof(UINT);
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(UINT);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = 3;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &result_)) ||
        FAILED(device->CreateUnorderedAccessView(result_.Get(), &view, &resultView_))) return false;

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& sample : samples_)
        if (FAILED(device->CreateBuffer(&desc, nullptr, &sample.staging))) return false;
    D3D11_BUFFER_DESC params{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    return SUCCEEDED(device->CreateBuffer(&params, nullptr, &params_));
}

void FlowMotionCheck::Reset()
{
    for (auto& sample : samples_) sample.pts = -1;
}

void FlowMotionCheck::Submit(ID3D11Device* device, ID3D11DeviceContext* context,
                            ID3D11ShaderResourceView* vectors, UINT width, UINT height, int64_t pts,
                            ID3D11ShaderResourceView* active, UINT grid)
{
    if (!vectors || !active || !width || !height || pts < 0) return;
    if (!tried_) {
        tried_ = true;
        if (!Initialize(device)) reduce_.Reset();
    }
    if (!reduce_) return;

    auto& sample = samples_[next_];
    next_ = (next_ + 1) % samples_.size();
    const UINT zero[4]{};
    context->ClearUnorderedAccessViewUint(resultView_.Get(), zero);
    auto* uav = resultView_.Get();
    auto* params = params_.Get();
    const UINT values[4]{grid};
    context->UpdateSubresource(params, 0, nullptr, values, 0, 0);
    context->CSSetConstantBuffers(0, 1, &params);
    context->CSSetShader(reduce_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* inputs[]{vectors, active};
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* empty[2]{};
    uav = nullptr;
    context->CSSetShaderResources(0, 2, empty);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(sample.staging.Get(), result_.Get());
    sample.pts = pts;
}

FlowMotion FlowMotionCheck::Take(ID3D11DeviceContext* context, int64_t pts)
{
    if (pts < 0) return FlowMotion::Unknown;
    for (auto& sample : samples_) {
        if (sample.pts != pts) continue;
        sample.pts = -1;
        D3D11_MAPPED_SUBRESOURCE data{};
        if (FAILED(context->Map(sample.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &data)))
            return FlowMotion::Unknown;
        const auto* counts = static_cast<const UINT*>(data.pData);
        const auto result = !counts[2] ? FlowMotion::Unknown :
            (uint64_t(counts[0]) * 10 >= counts[2] || counts[1] != 0 ? FlowMotion::Active : FlowMotion::Quiet);
        context->Unmap(sample.staging.Get(), 0);
        return result;
    }
    return FlowMotion::Unknown;
}

} // namespace mfg
