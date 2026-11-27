// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "DepthGpu.h"
#include "DepthGpuShaders.h"
#include "Edition/FixedShaderBytecode.h"
#include "DepthLookahead.h"
#include "DepthNative.h"
#include "DepthPack.h"
#include "DepthSplitPack.h"
#include "DepthStreamContract.h"
#include "DepthJobQueue.h"

#include <d3dcompiler.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "d3d12.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr unsigned RangeEntries = 6 + 3 * 1024; // Extrema, validity, scale reference, tile extrema and previous RGB.
constexpr unsigned RangeBytes = RangeEntries * sizeof(uint32_t);
constexpr unsigned RawTilePixels = 1024; // Must match DepthGpuShaders.h.
static_assert(2 * ((DepthPack::Pixels + RawTilePixels - 1) / RawTilePixels) <= 2 * 1024,
    "Raw depth tile extrema must fit before the scene RGB history");
constexpr unsigned TimestampCount = 4;
constexpr size_t TimestampOffset = sizeof(uint64_t);
constexpr size_t ReadbackBytes = TimestampOffset + TimestampCount * sizeof(uint64_t);
constexpr auto QueueType = D3D12_COMMAND_LIST_TYPE_COMPUTE;

bool PinModuleForBackgroundShutdown()
{
    static std::once_flag once;
    static bool pinned = false;
    std::call_once(once, [] {
        HMODULE module = nullptr;
        pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&PinModuleForBackgroundShutdown), &module) != FALSE;
    });
    return pinned;
}

void Check(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error(std::format("HRESULT 0x{:08X}", unsigned(hr)));
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

struct SharedTexture {
    ComPtr<ID3D11Texture2D> texture11;
    ComPtr<ID3D12Resource> texture12;

    void Create(ID3D11Device1* device11, ID3D12Device* device12, SIZE size, DXGI_FORMAT format, UINT flags)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size.cx;
        desc.Height = size.cy;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.BindFlags = flags;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Check(device11->CreateTexture2D(&desc, nullptr, &texture11));
        ComPtr<IDXGIResource1> resource;
        Check(texture11.As(&resource));
        HANDLE handle = nullptr;
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &handle));
        const auto opened = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&texture12));
        CloseHandle(handle);
        Check(opened);
    }
};

}

struct CDepthGpu::Impl {
    enum class State { Loading, Ready, Failed, Stopping };
    SIZE modelSize{};
    const unsigned slots = depthsettings::LookaheadMs()
        ? std::min(Slots, (depthsettings::LookaheadMs() * 60 + 999) / 1000 + 20) : NormalSlots;
    using Job = depthjobs::Queue<Result>::Job;

    ComPtr<ID3D11Device1> device11;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> captureFence, completeFence;
    ComPtr<ID3D11Fence> captureFence11;
    ComPtr<ID3D12CommandAllocator> packAllocator, postAllocator;
    ComPtr<ID3D12GraphicsCommandList> packList, postList;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> packPipeline, rangePipeline, normalizePipeline, rawTilePipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12QueryHeap> timestamps;
    uint64_t timestampFrequency = 0;
    ComPtr<ID3D12Resource> image, prediction, range, validity;
    std::array<SharedTexture, 2> captures;
    std::array<SharedTexture, Slots> outputs;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> captureViews;
    std::array<ComPtr<ID3D11ShaderResourceView>, Slots> views;
    UINT descriptorSize = 0;
    HANDLE completeEvent = nullptr;

    mutable std::mutex lock;
    std::condition_variable wake;
    std::atomic<State> state = State::Loading;
    std::atomic<int64_t> sceneStart = -1;
    const wchar_t* failure = L"DirectML initialization failed";
    std::thread worker;
    std::atomic<bool> workerFinished = false;
    depthjobs::Queue<Result> jobs;
    uint64_t serial = 0;
    uint64_t sceneSampleGeneration = 0;
    int64_t sceneSamplePts = -1;
    int64_t sceneSampleCut = -1;
    bool streamValid = false;
    uint64_t streamGeneration = 0;
    int64_t streamPts = -1;

    void RequestStop()
    {
        {
            std::lock_guard guard(lock);
            state = State::Stopping;
            jobs.CancelPending();
        }
        wake.notify_one();
    }

    ~Impl()
    {
        RequestStop();
        if (worker.joinable()) worker.join();
        if (completeEvent) CloseHandle(completeEvent);
    }

    ComPtr<ID3D12Resource> Buffer(UINT bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES initial,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = type;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> buffer;
        Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initial,
            nullptr, IID_PPV_ARGS(&buffer)));
        return buffer;
    }

    void CreateList(ComPtr<ID3D12CommandAllocator>& allocator, ComPtr<ID3D12GraphicsCommandList>& list)
    {
        Check(device->CreateCommandAllocator(QueueType, IID_PPV_ARGS(&allocator)));
        Check(device->CreateCommandList(0, QueueType, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        Check(list->Close());
    }

    template<edition::FixedShaderString Entry>
    ComPtr<ID3D12PipelineState> Pipeline()
    {
        ComPtr<ID3DBlob> code, errors;
        const auto hr = edition::CompileFixedShader<DepthGpuShaders,Entry,"cs_5_0">(&code, &errors);
        if (FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(hr);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pipeline;
        Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
        return pipeline;
    }

    void InitializeGpu()
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        Check(device11.As(&dxgi));
        Check(dxgi->GetAdapter(&adapter));
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = QueueType;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&captureFence)));
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completeFence)));
        completeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completeEvent) throw std::runtime_error("Cannot create DirectML completion event");
        ComPtr<ID3D11Device5> device5;
        Check(device11.As(&device5));
        HANDLE handle = nullptr;
        Check(device->CreateSharedHandle(captureFence.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
        const auto opened = device5->OpenSharedFence(handle, IID_PPV_ARGS(&captureFence11));
        CloseHandle(handle);
        Check(opened);

        constexpr unsigned captureBanks = 2;
        for (unsigned bank = 0; bank < captureBanks; ++bank) {
            captures[bank].Create(device11.Get(), device.Get(), modelSize, DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            Check(device11->CreateRenderTargetView(captures[bank].texture11.Get(), nullptr, &captureViews[bank]));
        }
        for (unsigned i = 0; i < slots; ++i) {
            outputs[i].Create(device11.Get(), device.Get(), modelSize, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
            Check(device11->CreateShaderResourceView(outputs[i].texture11.Get(), nullptr, &views[i]));
        }
        constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr auto allowUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto Pixels = UINT(modelSize.cx) * UINT(modelSize.cy);
        const auto TensorBytes = Pixels * sizeof(float);
        image = Buffer(3 * TensorBytes, D3D12_HEAP_TYPE_DEFAULT, uav, allowUav);
        prediction = Buffer(TensorBytes, D3D12_HEAP_TYPE_DEFAULT, uav, allowUav);
        range = Buffer(RangeBytes, D3D12_HEAP_TYPE_DEFAULT, uav, allowUav);
        const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, TimestampCount, 0};
        if (SUCCEEDED(queue->GetTimestampFrequency(&timestampFrequency)) && timestampFrequency)
            device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps));
        validity = Buffer(timestamps ? ReadbackBytes : sizeof(uint32_t),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        const D3D12_DESCRIPTOR_RANGE ranges[] = {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 1}
        };
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {UINT(std::size(ranges)), ranges};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 3};
        const D3D12_ROOT_SIGNATURE_DESC signature{UINT(std::size(params)), params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr));
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
        packPipeline = Pipeline<"Pack">();
        rawTilePipeline = Pipeline<"RawRangeTiles">();
        rangePipeline = Pipeline<"RawRange">();
        normalizePipeline = Pipeline<"RawNormalize">();
        CreateList(packAllocator, packList);
        CreateList(postAllocator, postList);

        // Each output slot has a stable descriptor table. The two command
        // lists can share it without changing descriptors in flight.
        constexpr auto type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{type, 5 * slots * captureBanks, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
        descriptorSize = device->GetDescriptorHandleIncrementSize(type);
        auto handleCpu = heap->GetCPUDescriptorHandleForHeapStart();
        for (unsigned bank = 0; bank < captureBanks; ++bank) {
            for (unsigned i = 0; i < slots; ++i) {
                auto& output = outputs[i];
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(captures[bank].texture12.Get(), &srv, handleCpu);
                handleCpu.ptr += descriptorSize;
                D3D12_UNORDERED_ACCESS_VIEW_DESC buffer{};
                buffer.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                buffer.Buffer.StructureByteStride = 4;
                buffer.Buffer.NumElements = 3 * Pixels;
                device->CreateUnorderedAccessView(image.Get(), nullptr, &buffer, handleCpu);
                handleCpu.ptr += descriptorSize;
                buffer.Buffer.NumElements = Pixels;
                device->CreateUnorderedAccessView(prediction.Get(), nullptr, &buffer, handleCpu);
                handleCpu.ptr += descriptorSize;
                D3D12_UNORDERED_ACCESS_VIEW_DESC texture{};
                texture.Format = DXGI_FORMAT_R32_FLOAT;
                texture.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device->CreateUnorderedAccessView(output.texture12.Get(), nullptr, &texture, handleCpu);
                handleCpu.ptr += descriptorSize;
                buffer.Buffer.NumElements = RangeEntries;
                device->CreateUnorderedAccessView(range.Get(), nullptr, &buffer, handleCpu);
                handleCpu.ptr += descriptorSize;
            }
        }
    }

    void Bind(ID3D12GraphicsCommandList* list, const Job& frame, int64_t cut, bool reuseScale = false)
    {
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.Get());
        auto descriptors = heap->GetGPUDescriptorHandleForHeapStart();
        descriptors.ptr += (uint64_t(frame.captureBank) * slots + frame.frame.slot) * 5 * descriptorSize;
        list->SetComputeRootDescriptorTable(0, descriptors);
        const bool hasPrevious = frame.frame.generation == sceneSampleGeneration &&
            sceneSamplePts >= 0 && frame.frame.pts > sceneSamplePts && !(sceneSampleCut < cut && cut <= frame.frame.pts);
        // Preserve the original RGB scene history decision. Scale history is
        // stricter: it must also belong to the last accepted W4 inference.
        const bool scaleHistory = hasPrevious && reuseScale && streamValid &&
            sceneSampleGeneration == streamGeneration && sceneSamplePts == streamPts;
        const UINT constants[]{UINT(hasPrevious) | (UINT(scaleHistory) << 1), UINT(modelSize.cx), UINT(modelSize.cy)};
        list->SetComputeRoot32BitConstants(1, UINT(std::size(constants)), constants, 0);
    }

    void Stamp(ID3D12GraphicsCommandList* list, unsigned index)
    {
        if (timestamps) list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void Execute(ID3D12GraphicsCommandList* list)
    {
        Check(list->Close());
        ID3D12CommandList* lists[]{list};
        queue->ExecuteCommandLists(1, lists);
    }

    void WaitForGpu(uint64_t value)
    {
        Check(queue->Signal(completeFence.Get(), value));
        if (completeFence->GetCompletedValue() < value) {
            Check(completeFence->SetEventOnCompletion(value, completeEvent));
            if (WaitForSingleObject(completeEvent, INFINITE) != WAIT_OBJECT_0)
                Check(E_FAIL);
        }
        Check(device->GetDeviceRemovedReason());
    }

    Result Infer(const Job& work, const DepthNative& model)
    {
        const auto started = std::chrono::steady_clock::now();
        int64_t cut = -1;
        const auto initialCut = sceneStart.load(std::memory_order_relaxed);
        const bool resetState = depthstream::NeedsReset(streamValid, streamGeneration, streamPts,
            work.frame.generation, work.frame.pts, initialCut);
        auto* capture = captures[work.captureBank].texture12.Get();
        Check(queue->Wait(captureFence.Get(), work.serial));
        try {
            Check(packAllocator->Reset());
            Check(packList->Reset(packAllocator.Get(), packPipeline.Get()));
            Stamp(packList.Get(), 0);
            Bind(packList.Get(), work, -1);
            constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
            constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            constexpr auto shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Transition(packList.Get(), capture, common, shaderRead);
            packList->Dispatch(modelSize.cx / 8, modelSize.cy / 8, 1);
            UavBarrier(packList.Get(), image.Get());
            Transition(packList.Get(), capture, shaderRead, common);
            Stamp(packList.Get(), 1);
            model.Record(packList.Get(), resetState);
            Stamp(packList.Get(), 2);
            Execute(packList.Get());

            Check(postAllocator->Reset());
            Check(postList->Reset(postAllocator.Get(), rawTilePipeline.Get()));
            cut = sceneStart.load(std::memory_order_relaxed);
            Bind(postList.Get(), work, cut, !resetState);
            UavBarrier(postList.Get(), prediction.Get());
            // Full-map extrema and same-sequence scale reference; no previous-map blending.
            const UINT pixels = UINT(modelSize.cx) * UINT(modelSize.cy);
            postList->Dispatch((pixels + RawTilePixels - 1) / RawTilePixels, 1, 1);
            UavBarrier(postList.Get(), range.Get());
            postList->SetPipelineState(rangePipeline.Get());
            Transition(postList.Get(), capture, common, shaderRead);
            postList->Dispatch(1, 1, 1);
            Transition(postList.Get(), capture, shaderRead, common);
            UavBarrier(postList.Get(), range.Get());
            auto* output = outputs[work.frame.slot].texture12.Get();
            Transition(postList.Get(), output, common, uav);
            postList->SetPipelineState(normalizePipeline.Get());
            postList->Dispatch(modelSize.cx / 8, modelSize.cy / 8, 1);
            Transition(postList.Get(), output, uav, common);
            constexpr auto copySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
            Transition(postList.Get(), range.Get(), uav, copySource);
            postList->CopyBufferRegion(validity.Get(), 0, range.Get(), 2 * sizeof(uint32_t), sizeof(uint32_t));
            Transition(postList.Get(), range.Get(), copySource, uav);
            Stamp(postList.Get(), 3);
            if (timestamps)
                postList->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                    TimestampCount, validity.Get(), TimestampOffset);
            Execute(postList.Get());
            WaitForGpu(work.serial);
        } catch (...) {
            // A failed run may already have submitted work using these resources.
            if (SUCCEEDED(device->GetDeviceRemovedReason())) WaitForGpu(work.serial + 1);
            throw;
        }
        void* mapped = nullptr;
        const D3D12_RANGE read{0, timestamps ? ReadbackBytes : sizeof(uint32_t)}, written{0, 0};
        Check(validity->Map(0, &read, &mapped));
        auto done = work.frame;
        const auto flags = *static_cast<const uint32_t*>(mapped);
        done.valid = (flags & 1u) != 0;
        done.sceneCut = (flags & 2u) != 0;
        if (timestamps) {
            const auto* ticks = reinterpret_cast<const uint64_t*>(static_cast<const char*>(mapped) + TimestampOffset);
            if (ticks[0] && ticks[0] <= ticks[1] && ticks[1] <= ticks[2] && ticks[2] <= ticks[3]) {
                const double msPerTick = 1000.0 / double(timestampFrequency);
                done.timings = {double(ticks[1] - ticks[0]) * msPerTick,
                    double(ticks[2] - ticks[1]) * msPerTick,
                    double(ticks[3] - ticks[2]) * msPerTick, true};
            }
        }
        validity->Unmap(0, &written);
        // A cut discovered after dispatch cannot retroactively change attention.
        // Do not publish that estimate; reset the next inference instead.
        const auto latestCut = sceneStart.load(std::memory_order_relaxed);
        const bool lateCut = !resetState && (done.sceneCut ||
            (streamPts < latestCut && latestCut <= done.pts));
        if (lateCut) {
            done.valid = false;
            sceneSamplePts = -1;
        }
        streamValid = done.valid;
        streamGeneration = done.generation;
        streamPts = done.pts;
        if (done.valid) {
            sceneSampleGeneration = done.generation;
            sceneSamplePts = done.pts;
            sceneSampleCut = done.sceneCut ? done.pts : cut <= done.pts ? cut : -1;
        }
        done.inferenceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return done;
    }

    void RunModel(const DepthNative& model)
    {
        {
            std::lock_guard guard(lock);
            if (state == State::Stopping) return;
            state = State::Ready;
        }
        for (;;) {
            Job work;
            {
                std::unique_lock guard(lock);
                wake.wait(guard, [this] { return state == State::Stopping || jobs.pending; });
                if (state == State::Stopping) break;
                work = jobs.Begin();
            }
            const auto done = Infer(work, model);
            {
                std::lock_guard guard(lock);
                jobs.Complete(done);
            }
        }
    }

    void Run(const std::filesystem::path& directory, const std::filesystem::path& modelPath)
    {
        const wchar_t* stage = L"DirectML model pack read failed";
        try {
            const auto pack = DepthPack::ReadFile(modelPath, DepthPack::Limit + depthsplit::PrefixSize);
            const auto pool = DepthPack::ReadFile(directory / depthsplit::WeightFile, DepthPack::Limit + depthsplit::PrefixSize);
            DepthPack::ValidateHeader(depthsplit::Payload(pack, depthsplit::GraphMagic));
            modelSize = {LONG(DepthPack::Width), LONG(DepthPack::Height)};
            stage = L"DirectML GPU resource setup failed";
            InitializeGpu();
            stage = L"DirectML model pack initialization failed";
            const DepthNative model(device.Get(), queue.Get(), image.Get(), prediction.Get(),
                pack, directory / L"DirectML.dll", pool);
            stage = L"DirectML inference failed";
            RunModel(model);
        } catch (const std::exception& error) {
            OutputDebugStringW(stage);
            OutputDebugStringA(": ");
            OutputDebugStringA(error.what());
            OutputDebugStringA("\n");
            std::lock_guard guard(lock);
            if (state != State::Stopping) {
                failure = stage;
                state = State::Failed;
            }
        }
        workerFinished.store(true, std::memory_order_release);
    }
};

std::mutex CDepthGpu::s_retiredMutex;
std::vector<std::unique_ptr<CDepthGpu::Impl>> CDepthGpu::s_retired;

void CDepthGpu::ReapRetired()
{
    std::lock_guard guard(s_retiredMutex);
    std::erase_if(s_retired, [](const auto& impl) {
        return impl->workerFinished.load(std::memory_order_acquire);
    });
}

void CDepthGpu::Retire(std::unique_ptr<Impl> impl)
{
    if (!impl) return;
    impl->RequestStop();
    std::lock_guard guard(s_retiredMutex);
    std::erase_if(s_retired, [](const auto& old) {
        return old->workerFinished.load(std::memory_order_acquire);
    });
    s_retired.emplace_back(std::move(impl));
}

CDepthGpu::CDepthGpu() = default;
CDepthGpu::~CDepthGpu() = default;

bool CDepthGpu::Start(ID3D11Device1* device, const std::filesystem::path& directory)
{
    ReapRetired();
    try {
        const auto modelPath = directory / depthsplit::GraphFileName;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(modelPath, ec) ||
            !std::filesystem::is_regular_file(directory / depthsplit::WeightFile, ec)) return false;
        m_impl = std::make_unique<Impl>();
        m_impl->device11 = device;
        m_impl->worker = std::thread([impl = m_impl.get(), directory, modelPath] { impl->Run(directory, modelPath); });
        return true;
    } catch (const std::exception&) {
        m_impl.reset();
        return false;
    }
}

void CDepthGpu::Stop(bool fastShutdown)
{
    if (!m_impl) return;
    if (fastShutdown && !m_impl->workerFinished.load(std::memory_order_acquire) &&
        PinModuleForBackgroundShutdown()) {
        Retire(std::move(m_impl));
        return;
    }
    m_impl.reset();
}

const wchar_t* CDepthGpu::Status(bool hasEstimate) const
{
    if (!m_impl) return L"DirectML model missing or unavailable";
    switch (m_impl->state.load()) {
        case Impl::State::Loading: return L"loading DirectML model";
        case Impl::State::Ready: return hasEstimate ? L"no estimate for this frame" : L"awaiting DirectML estimate";
        case Impl::State::Failed: return m_impl->failure;
        default: return L"DirectML stopping";
    }
}

bool CDepthGpu::Loading() const
{
    return m_impl && m_impl->state == Impl::State::Loading;
}

void CDepthGpu::SetSceneStart(int64_t pts)
{
    if (!m_impl) return;
    std::lock_guard guard(m_impl->lock);
    m_impl->sceneStart.store(pts, std::memory_order_relaxed);
    if (m_impl->jobs.pending && (pts < 0 || m_impl->jobs.pendingJob.frame.pts < pts))
        m_impl->jobs.CancelPending();
}

bool CDepthGpu::CanAccept() const
{
    if (!m_impl || m_impl->state != Impl::State::Ready) return false;
    std::lock_guard guard(m_impl->lock);
    return m_impl->jobs.CanAccept();
}
bool CDepthGpu::CanStart() const
{
    if (!m_impl) return false;
    const auto state = m_impl->state.load();
    return state == Impl::State::Loading || state == Impl::State::Ready;
}
unsigned CDepthGpu::Capacity() const { return m_impl ? m_impl->slots : 0; }

bool CDepthGpu::SlotBusy(unsigned slot) const
{
    if (!m_impl) return true;
    std::lock_guard guard(m_impl->lock);
    return m_impl->jobs.SlotBusy(slot);
}

ID3D11RenderTargetView* CDepthGpu::Target(unsigned& bank) const
{
    if (!m_impl) return nullptr;
    std::lock_guard guard(m_impl->lock);
    if (m_impl->state != Impl::State::Ready || !m_impl->jobs.CanAccept()) return nullptr;
    bank = m_impl->jobs.CaptureBank();
    return m_impl->captureViews[bank].Get();
}
SIZE CDepthGpu::ModelSize() const
{
    return m_impl && m_impl->state == Impl::State::Ready ? m_impl->modelSize : SIZE{};
}
ID3D11ShaderResourceView* CDepthGpu::View(unsigned slot) const { return m_impl->views[slot].Get(); }

bool CDepthGpu::Submit(ID3D11DeviceContext1* context, Result frame, unsigned bank)
{
    ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4)))) return false;
    std::lock_guard guard(m_impl->lock);
    if (m_impl->state != Impl::State::Ready || frame.slot >= m_impl->slots ||
        !m_impl->jobs.CanSubmit(bank, frame.slot)) return false;
    const auto serial = ++m_impl->serial;
    // This orders both the capture and all earlier D3D11 reads of the output slot.
    if (FAILED(context4->Signal(m_impl->captureFence11.Get(), serial))) return false;
    context->Flush();
    m_impl->jobs.Submit({frame, serial, bank});
    m_impl->wake.notify_one();
    return true;
}

bool CDepthGpu::Take(Result& result)
{
    if (!m_impl) return false;
    std::lock_guard guard(m_impl->lock);
    return m_impl->jobs.Take(result);
}
