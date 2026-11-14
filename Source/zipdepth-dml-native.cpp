#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define DML_TARGET_VERSION_USE_LATEST
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "DirectML-abi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "inputs.hpp"

__CRT_UUID_DECL(IDMLDevice1, 0xa0884f9a, 0xd2be, 0x4355, 0xaa, 0x5d, 0x59, 0x01, 0x28, 0x1a, 0xd1, 0xd2)
__CRT_UUID_DECL(IDMLOperator, 0x26caae7a, 0x3081, 0x4633, 0x95, 0x81, 0x22, 0x6f, 0xbe, 0x57, 0x69, 0x5d)
__CRT_UUID_DECL(IDMLCompiledOperator, 0x6b15e56a, 0xbf5c, 0x4902, 0x92, 0xd8, 0xda, 0x3a, 0x65, 0x0a, 0xfe, 0xa4)
__CRT_UUID_DECL(IDMLOperatorInitializer, 0x427c1113, 0x435c, 0x469c, 0x86, 0x76, 0x4d, 0x5d, 0xd0, 0x72, 0xf8, 0x13)
__CRT_UUID_DECL(IDMLBindingTable, 0x29c687dc, 0xde74, 0x4e3b, 0xab, 0x00, 0x11, 0x68, 0xf2, 0xfc, 0x3c, 0xfc)
__CRT_UUID_DECL(IDMLCommandRecorder, 0xe6857a76, 0x2e3e, 0x4fdd, 0xbf, 0xf4, 0x5d, 0x2b, 0xa1, 0x0f, 0xb4, 0x53)

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

[[noreturn]] void FailHr(HRESULT hr, const char* stage)
{
    std::cerr << stage << " HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
    throw std::runtime_error(stage);
}

void Check(HRESULT hr, const char* stage)
{
    if (FAILED(hr)) FailHr(hr, stage);
}
#include "native-graph.hpp"

struct DmlLibrary {
    HMODULE handle;
    explicit DmlLibrary(const fs::path& dll)
        : handle(LoadLibraryExW(dll.c_str(), nullptr,
                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
    {
        if (!handle) Check(HRESULT_FROM_WIN32(GetLastError()), "LoadLibraryExW DirectML.dll");
    }
    DmlLibrary(const DmlLibrary&) = delete;
    ~DmlLibrary() { FreeLibrary(handle); }
};

ComPtr<IDXGIAdapter1> PickAdapter()
{
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> firstHardware;
    ComPtr<IDXGIAdapter1> firstNvidia;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        Check(hr, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 desc{};
        Check(adapter->GetDesc1(&desc), "GetDesc1");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!firstHardware) firstHardware = adapter;
        if (desc.VendorId == 0x10de && !firstNvidia) firstNvidia = adapter;
        if (desc.VendorId == 0x10de && desc.DeviceId == 0x2803) return adapter;
    }
    if (firstNvidia) return firstNvidia;
    if (firstHardware) return firstHardware;
    throw std::runtime_error("No hardware DXGI adapter");
}

double Milliseconds(Clock::duration elapsed)
{
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

std::vector<std::uint8_t> ReadFile(const fs::path& path, std::size_t limit = 64 * 1024 * 1024)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > limit) {
        std::wcout << L"File: " << path << std::endl;
        throw std::runtime_error("Missing, empty or oversized file");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        throw std::runtime_error("Unable to read file");
    }
    return bytes;
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> commands;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<IDMLDevice1> dml;
    ComPtr<IDMLCommandRecorder> recorder;
    std::unique_ptr<void, decltype(&CloseHandle)> event{nullptr, CloseHandle};
    std::uint64_t fenceValue = 0;

    explicit Gpu(HMODULE library)
    {
        const auto adapter = PickAdapter();
        DXGI_ADAPTER_DESC1 desc{};
        Check(adapter->GetDesc1(&desc), "GetDesc1");
        std::wcout << L"Adapter: " << desc.Description << std::endl;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())),
              "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.GetAddressOf())), "CreateCommandQueue");
        Check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(commands.GetAddressOf())),
              "CreateCommandAllocator");
        Check(device->CreateCommandList(0, queueDesc.Type, commands.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf())),
              "CreateCommandList");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())), "CreateFence");
        event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!event) Check(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
        const auto create = reinterpret_cast<decltype(&DMLCreateDevice)>(GetProcAddress(library, "DMLCreateDevice"));
        if (!create) throw std::runtime_error("Missing DMLCreateDevice");
        Check(create(device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(dml.GetAddressOf())), "DMLCreateDevice");
        Check(dml->CreateCommandRecorder(IID_PPV_ARGS(recorder.GetAddressOf())), "CreateCommandRecorder");
    }

    void CheckDevice(HRESULT hr, const char* stage) const
    {
        if (SUCCEEDED(hr)) return;
        const auto reason = device->GetDeviceRemovedReason();
        if (FAILED(reason)) {
            std::cerr << "GetDeviceRemovedReason: 0x" << std::hex
                      << static_cast<unsigned long>(reason) << std::dec << '\n';
        }
        FailHr(hr, stage);
    }

    void CheckDeviceState(const char* stage) const
    {
        Check(device->GetDeviceRemovedReason(), stage);
    }

    ComPtr<ID3D12Resource> Buffer(std::uint64_t bytes, D3D12_HEAP_TYPE heap,
                                  D3D12_RESOURCE_STATES state, const char* stage) const
    {
        if (!bytes) return {};
        if (bytes > 512ull * 1024 * 1024) {
            std::cerr << stage << ": requested " << bytes << " bytes\n";
            throw std::runtime_error("Buffer size exceeds this checker's 512 MiB limit");
        }
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (heap == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;
        const auto hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(result.GetAddressOf()));
        if (FAILED(hr)) {
            std::cerr << "CreateCommittedResource at " << stage << ": " << bytes
                      << " bytes, heap " << heap << '\n';
        }
        CheckDevice(hr, stage);
        return result;
    }

    ComPtr<ID3D12Resource> DeviceBuffer(std::uint64_t bytes, const char* stage) const
    {
        return Buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, stage);
    }

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) const
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &barrier);
    }

    void UavBarrier(ID3D12Resource* resource) const
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        list->ResourceBarrier(1, &barrier);
    }

    void WaitAndReset(const char* stage)
    {
        CheckDeviceState(stage);
        CheckDevice(list->Close(), "Close command list");
        ID3D12CommandList* submission[] = {list.Get()};
        queue->ExecuteCommandLists(1, submission);
        CheckDevice(queue->Signal(fence.Get(), ++fenceValue), "Signal fence");
        CheckDevice(fence->SetEventOnCompletion(fenceValue, event.get()), "SetEventOnCompletion");
        if (WaitForSingleObject(event.get(), 30000) != WAIT_OBJECT_0) {
            throw std::runtime_error("GPU fence wait failed or timed out");
        }
        CheckDeviceState(stage);
        Check(commands->Reset(), "Reset command allocator");
        Check(list->Reset(commands.Get(), nullptr), "Reset command list");
    }
};

struct BoundTable {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<IDMLBindingTable> table;
    BoundTable(const Gpu& gpu, IDMLDispatchable* dispatch)
    {
        const UINT count = dispatch->GetBindingProperties().RequiredDescriptorCount;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = std::max(1u, count);
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        gpu.CheckDevice(gpu.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())), "CreateDescriptorHeap");
        const DML_BINDING_TABLE_DESC layout{dispatch, heap->GetCPUDescriptorHandleForHeapStart(),
                                          heap->GetGPUDescriptorHandleForHeapStart(), count};
        gpu.CheckDevice(gpu.dml->CreateBindingTable(&layout, IID_PPV_ARGS(table.GetAddressOf())), "CreateBindingTable");
    }
    void Record(const Gpu& gpu, IDMLDispatchable* dispatch) const
    {
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        gpu.list->SetDescriptorHeaps(1, heaps);
        gpu.recorder->RecordDispatch(gpu.list.Get(), dispatch, table.Get());
    }
};

void WriteUpload(ID3D12Resource* resource, const void* data, std::size_t size)
{
    const D3D12_RANGE empty{0, 0};
    void* mapped = nullptr;
    Check(resource->Map(0, &empty, &mapped), "Map upload buffer");
    std::memcpy(mapped, data, size);
    const D3D12_RANGE written{0, size};
    resource->Unmap(0, &written);
}

struct CompiledGraph {
    ComPtr<IDMLCompiledOperator> dispatch;
    ComPtr<ID3D12Resource> persistent;
    UINT inputSlots = 0;
};

CompiledGraph CompileAndInitialize(Gpu& gpu, const fs::path& graphPath)
{
    const auto file = ReadFile(graphPath);
    flatbuffers::Verifier verifier(file.data(), file.size());
    if (!dml::ir::VerifyDmlGraphDescBuffer(verifier)) throw std::runtime_error("Invalid DirectML graph");
    const auto* root = dml::ir::GetDmlGraphDesc(file.data());
    if (!root->nodes() || !root->graphInputNames() || !root->graphOutputNames() ||
        root->graphOutputNames()->size() != 1 || root->graphOutputNames()->Get(0)->str() != "depth") {
        throw std::runtime_error("Expected the exported ZipDepth image/depth graph");
    }
    std::unordered_map<std::uint32_t, std::uint32_t> imageSlots;
    for (std::uint32_t i = 0; i < root->graphInputNames()->size(); ++i) {
        const auto name = root->graphInputNames()->Get(i)->string_view();
        if (name.empty()) continue; // ORT sizes the name vector by input edges, which can leave empty slots.
        if (name != "image" || !imageSlots.empty()) throw std::runtime_error("Unexpected graph input name");
        imageSlots.emplace(i, 0);
    }
    std::vector<std::unique_ptr<std::byte[]>> constants;
    const auto graph = DeserializeDmlGraph(file.data(), constants);
    if (imageSlots.empty() || graph.InputEdges.empty() || graph.OutputEdges.size() != 1) {
        throw std::runtime_error("Unexpected graph input or output count");
    }
    constexpr UINT dynamicInputs = 1;

    // Assign named constants in edge order so initialization and compilation use the same input slots.
    std::unordered_map<std::string_view, std::uint32_t> slots;
    std::vector<std::string_view> filenames;
    for (const auto& edge : graph.IntermediateEdges) {
        const auto& node = graph.Nodes.at(edge.FromNodeIndex);
        const auto* constant = std::get_if<DmlSerializedGraphNodeConstantVariant>(&node.Desc);
        if (!constant) continue;
        const auto* external = std::get_if<ConstantName>(constant);
        if (!external) continue;
        if (slots.emplace(node.Name, dynamicInputs + static_cast<UINT>(filenames.size())).second) {
            filenames.emplace_back(external->name);
        }
    }

    BucketAllocator allocator;
    DML_GRAPH_DESC desc{};
    std::vector<ComPtr<IDMLOperator>> operators;
    std::vector<DML_GRAPH_NODE_DESC> nodes;
    std::vector<DML_GRAPH_EDGE_DESC> inputs, outputs, intermediate;
    ConvertGraphDesc(graph, dynamicInputs, graph.OutputCount, gpu.dml.Get(), allocator,
                     &imageSlots, &slots, desc, operators, nodes, inputs, outputs, intermediate);
    if (desc.InputCount != dynamicInputs + filenames.size()) {
        throw std::runtime_error("Graph binding slots do not match external weights");
    }

    CompiledGraph result;
    result.inputSlots = desc.InputCount;
    const auto compileStart = Clock::now();
    Check(gpu.dml->CompileGraph(&desc, DML_EXECUTION_FLAG_NONE, IID_PPV_ARGS(result.dispatch.GetAddressOf())),
          "CompileGraph");
    std::wcout << L"Native CompileGraph: " << Milliseconds(Clock::now() - compileStart) << L" ms"
               << L", operators " << operators.size() << L", external weights " << filenames.size() << std::endl;

    gpu.CheckDeviceState("After CompileGraph");
    const auto dispatchProps = result.dispatch->GetBindingProperties();
    gpu.CheckDeviceState("After compiled graph GetBindingProperties");
    std::wcout << L"Graph bindings: " << dispatchProps.RequiredDescriptorCount
               << L" descriptors, scratch " << dispatchProps.TemporaryResourceSize
               << L" bytes, persistent " << dispatchProps.PersistentResourceSize << L" bytes" << std::endl;

    std::vector<ComPtr<ID3D12Resource>> weights, staging;
    std::vector<DML_BUFFER_BINDING> initialInputs(desc.InputCount);
    for (std::size_t i = 0; i < filenames.size(); ++i) {
        const std::string_view name = filenames[i];
        if (name.empty() || name.find_first_of("<>:\"/\\|?*") != std::string_view::npos || name == "." || name == "..") {
            throw std::runtime_error("Invalid external weight file name");
        }
        const auto bytes = ReadFile(graphPath.parent_path() / (std::string(name) + ".bin"));
        if (bytes.size() % 4) throw std::runtime_error("Unaligned weight file size");
        auto upload = gpu.Buffer(bytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, "Weight upload");
        auto weight = gpu.Buffer(bytes.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, "Weight GPU buffer");
        WriteUpload(upload.Get(), bytes.data(), bytes.size());
        gpu.list->CopyBufferRegion(weight.Get(), 0, upload.Get(), 0, bytes.size());
        gpu.Transition(weight.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        initialInputs[dynamicInputs + i] = {weight.Get(), 0, bytes.size()};
        staging.push_back(std::move(upload));
        weights.push_back(std::move(weight));
    }

    ComPtr<IDMLOperatorInitializer> initializer;
    IDMLCompiledOperator* compiled[] = {result.dispatch.Get()};
    gpu.CheckDevice(gpu.dml->CreateOperatorInitializer(1, compiled, IID_PPV_ARGS(initializer.GetAddressOf())),
          "CreateOperatorInitializer");
    const auto initProps = initializer->GetBindingProperties();
    gpu.CheckDeviceState("After initializer GetBindingProperties");
    std::wcout << L"Initializer bindings: " << initProps.RequiredDescriptorCount
               << L" descriptors, scratch " << initProps.TemporaryResourceSize << L" bytes" << std::endl;
    auto scratch = gpu.DeviceBuffer(initProps.TemporaryResourceSize, "Initializer scratch");
    result.persistent = gpu.DeviceBuffer(dispatchProps.PersistentResourceSize, "Graph persistent");
    const DML_BUFFER_ARRAY_BINDING weightsBinding{desc.InputCount, initialInputs.data()};
    const DML_BINDING_DESC inputBinding{DML_BINDING_TYPE_BUFFER_ARRAY, &weightsBinding};
    const DML_BUFFER_BINDING tempBinding{scratch.Get(), 0, initProps.TemporaryResourceSize};
    const DML_BINDING_DESC tempDesc{DML_BINDING_TYPE_BUFFER, &tempBinding};
    const DML_BUFFER_BINDING persistent{result.persistent.Get(), 0, dispatchProps.PersistentResourceSize};
    const DML_BINDING_DESC initOutput = result.persistent ? DML_BINDING_DESC{DML_BINDING_TYPE_BUFFER, &persistent}
                                                         : DML_BINDING_DESC{DML_BINDING_TYPE_NONE, nullptr};
    const BoundTable table(gpu, initializer.Get());
    table.table->BindInputs(1, &inputBinding);
    table.table->BindOutputs(1, &initOutput);
    if (scratch) table.table->BindTemporaryResource(&tempDesc);
    gpu.CheckDeviceState("After binding initializer resources");
    table.Record(gpu, initializer.Get());
    if (result.persistent) gpu.UavBarrier(result.persistent.Get());
    gpu.WaitAndReset("Graph initializer dispatch");
    return result;
}

struct Inference {
    static constexpr std::size_t inputBytes = 3 * Pixels * sizeof(float);
    static constexpr std::size_t outputBytes = Pixels * sizeof(float);

    Gpu& gpu;
    IDMLCompiledOperator* dispatch;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> image;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> scratch;
    BoundTable bindings;

    Inference(Gpu& gpu, const CompiledGraph& graph)
        : gpu(gpu), dispatch(graph.dispatch.Get()),
          upload(gpu.Buffer(inputBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, "Image upload")),
          image(gpu.DeviceBuffer(inputBytes, "Image tensor")), depth(gpu.DeviceBuffer(outputBytes, "Depth tensor")),
          readback(gpu.Buffer(outputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, "Depth readback")),
          scratch(gpu.DeviceBuffer(dispatch->GetBindingProperties().TemporaryResourceSize, "Inference scratch")),
          bindings(gpu, dispatch)
    {
        const auto props = dispatch->GetBindingProperties();
        const DML_BUFFER_BINDING imageData{image.Get(), 0, inputBytes};
        const DML_BUFFER_BINDING depthData{depth.Get(), 0, outputBytes};
        const DML_BUFFER_BINDING temporary{scratch.Get(), 0, props.TemporaryResourceSize};
        const DML_BUFFER_BINDING persistent{graph.persistent.Get(), 0, props.PersistentResourceSize};
        std::vector<DML_BINDING_DESC> input(graph.inputSlots, {DML_BINDING_TYPE_NONE, nullptr});
        input.front() = {DML_BINDING_TYPE_BUFFER, &imageData};
        const DML_BINDING_DESC output{DML_BINDING_TYPE_BUFFER, &depthData};
        const DML_BINDING_DESC temp{DML_BINDING_TYPE_BUFFER, &temporary};
        const DML_BINDING_DESC state{DML_BINDING_TYPE_BUFFER, &persistent};
        bindings.table->BindInputs(static_cast<UINT>(input.size()), input.data());
        bindings.table->BindOutputs(1, &output);
        if (scratch) bindings.table->BindTemporaryResource(&temp);
        if (graph.persistent) bindings.table->BindPersistentResource(&state);
        gpu.CheckDeviceState("After binding inference resources");
    }

    void Run(const std::vector<float>& input, std::vector<float>& output) const
    {
        WriteUpload(upload.Get(), input.data(), inputBytes);
        gpu.Transition(image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.list->CopyBufferRegion(image.Get(), 0, upload.Get(), 0, inputBytes);
        gpu.Transition(image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bindings.Record(gpu, dispatch);
        if (scratch) gpu.UavBarrier(scratch.Get());
        gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyBufferRegion(readback.Get(), 0, depth.Get(), 0, outputBytes);
        gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.WaitAndReset("Inference dispatch");
        void* data = nullptr;
        const D3D12_RANGE readable{0, outputBytes};
        Check(readback->Map(0, &readable, &data), "Map depth buffer");
        std::memcpy(output.data(), data, outputBytes);
        const D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
    }
};

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc < 2 || argc > 3 || std::wstring_view(argv[1]) == L"--help") {
            std::wcout << L"Usage: zipdepth-dml-native.exe REPORT_FOLDER [DirectML.dll path]\n";
            return argc < 2 || argc > 3 ? 1 : 0;
        }
        const auto report = fs::absolute(argv[1]);
        const auto graphPath = report / L"zipdepth_edgepad_384_mixed" / L"Partition_0.bin";
        const auto dll = fs::absolute(argc == 3 ? fs::path(argv[2]) : fs::path(L"dist/dlss/zipdepth/DirectML.dll"));
        std::wcout << L"ZipDepth native checker v3 (Windows COM ABI)\nGraph: " << graphPath << L"\nDirectML: " << dll << std::endl;

        const auto start = Clock::now();
        const DmlLibrary library(dll);
        Gpu gpu(library.handle);
        const auto graph = CompileAndInitialize(gpu, graphPath);
        const Inference inference(gpu, graph);
        std::wcout << L"Native startup to ready: " << Milliseconds(Clock::now() - start) << L" ms" << std::endl;
        bool matched = true;
        std::vector<float> result(Pixels);
        for (int index = 0; index < InputCount; ++index) {
            const auto file = ReadFile(report / ("ort-depth-" + std::to_string(index) + ".f32"), Inference::outputBytes);
            if (file.size() != Inference::outputBytes) throw std::runtime_error("Incorrect reference tensor size");
            std::vector<float> reference(Pixels);
            std::memcpy(reference.data(), file.data(), file.size());
            inference.Run(MakeInput(index), result);
            float difference = 0;
            for (std::size_t i = 0; i < result.size(); ++i) {
                if (!std::isfinite(result[i]) || !std::isfinite(reference[i])) {
                    throw std::runtime_error("Non-finite depth sample");
                }
                difference = std::max(difference, std::abs(result[i] - reference[i]));
            }
            std::wcout << L"Input " << index << L" depth max absolute difference: "
                       << std::setprecision(9) << difference << std::endl;
            matched &= difference <= 0.001f;
        }

        const auto input = MakeInput(InputCount - 1);
        std::array<double, 15> timings{};
        for (auto& time : timings) {
            const auto begin = Clock::now();
            inference.Run(input, result);
            time = Milliseconds(Clock::now() - begin);
        }
        std::sort(timings.begin(), timings.end());
        std::wcout << L"Native warm CPU-to-CPU median: " << timings[timings.size() / 2] << L" ms\n";
        const bool ortLoaded = GetModuleHandleW(L"onnxruntime.dll") ||
                               GetModuleHandleW(L"onnxruntime_providers_shared.dll");
        std::wcout << L"ONNX Runtime loaded: " << (ortLoaded ? L"yes" : L"no") << L'\n';
        if (matched && !ortLoaded) {
            std::wcout << L"RESULT: NATIVE_DIRECTML_MATCH\n";
            return 0;
        }
        std::wcout << L"RESULT: NATIVE_DIRECTML_NOT_CONFIRMED\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << "\nRESULT: NATIVE_CHECK_FAILED\n";
        return 1;
    }
}
