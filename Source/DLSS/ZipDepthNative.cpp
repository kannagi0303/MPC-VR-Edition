// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#define DML_TARGET_VERSION_USE_LATEST
#ifdef __MINGW32__
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "DirectML-abi.h"
#else
#include <DirectML.h>
#endif
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <stdexcept>

#include "ZipDepthNative.h"
#include "ZipDepthPack.h"

using Microsoft::WRL::ComPtr;

#ifdef __MINGW32__
__CRT_UUID_DECL(IDMLDevice1, 0xa0884f9a, 0xd2be, 0x4355, 0xaa, 0x5d, 0x59, 0x01, 0x28, 0x1a, 0xd1, 0xd2)
__CRT_UUID_DECL(IDMLOperator, 0x26caae7a, 0x3081, 0x4633, 0x95, 0x81, 0x22, 0x6f, 0xbe, 0x57, 0x69, 0x5d)
__CRT_UUID_DECL(IDMLCompiledOperator, 0x6b15e56a, 0xbf5c, 0x4902, 0x92, 0xd8, 0xda, 0x3a, 0x65, 0x0a, 0xfe, 0xa4)
__CRT_UUID_DECL(IDMLOperatorInitializer, 0x427c1113, 0x435c, 0x469c, 0x86, 0x76, 0x4d, 0x5d, 0xd0, 0x72, 0xf8, 0x13)
__CRT_UUID_DECL(IDMLBindingTable, 0x29c687dc, 0xde74, 0x4e3b, 0xab, 0x00, 0x11, 0x68, 0xf2, 0xfc, 0x3c, 0xfc)
__CRT_UUID_DECL(IDMLCommandRecorder, 0xe6857a76, 0x2e3e, 0x4fdd, 0xbf, 0xf4, 0x5d, 0x2b, 0xa1, 0x0f, 0xb4, 0x53)
#endif

namespace {
[[noreturn]] void Fail(HRESULT hr)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "HRESULT 0x%08X", static_cast<unsigned>(hr));
    throw std::runtime_error(text);
}

void Check(HRESULT hr)
{
    if (FAILED(hr)) Fail(hr);
}
}

#include "DirectMLGraph/GraphHelpers.h"

namespace {
constexpr unsigned TensorBytes = ZipDepthPack::Side * ZipDepthPack::Side * sizeof(float);
constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto CopyDest = D3D12_RESOURCE_STATE_COPY_DEST;

struct Library {
    HMODULE handle;
    explicit Library(const std::filesystem::path& path)
        : handle(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        if (!handle) Check(HRESULT_FROM_WIN32(GetLastError()));
    }
    ~Library() { FreeLibrary(handle); }
    Library(const Library&) = delete;
};

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type,
    D3D12_RESOURCE_STATES state)
{
    if (!bytes) return {};
    if (bytes > std::numeric_limits<UINT>::max())
        throw std::runtime_error("ZipDepth DirectML resource exceeds the supported buffer size");
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
        IID_PPV_ARGS(result.GetAddressOf())));
    return result;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = buffer;
    list->ResourceBarrier(1, &barrier);
}

struct Bindings {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<IDMLBindingTable> table;

    void Create(ID3D12Device* gpu, IDMLDevice* dml, IDMLDispatchable* dispatch)
    {
        const auto count = dispatch->GetBindingProperties().RequiredDescriptorCount;
        if (count > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1)
            throw std::runtime_error("Invalid DirectML descriptor count");
        const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            std::max(1u, count), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Check(gpu->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())));
        const DML_BINDING_TABLE_DESC layout{dispatch, heap->GetCPUDescriptorHandleForHeapStart(),
            heap->GetGPUDescriptorHandleForHeapStart(), count};
        Check(dml->CreateBindingTable(&layout, IID_PPV_ARGS(table.GetAddressOf())));
    }

    void Record(ID3D12GraphicsCommandList* list, IDMLCommandRecorder* recorder, IDMLDispatchable* dispatch) const
    {
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        recorder->RecordDispatch(list, dispatch, table.Get());
    }
};

DML_BINDING_DESC Binding(const DML_BUFFER_BINDING& buffer)
{
    return buffer.Buffer ? DML_BINDING_DESC{DML_BINDING_TYPE_BUFFER, &buffer}
                         : DML_BINDING_DESC{DML_BINDING_TYPE_NONE, nullptr};
}

void CheckTensor(const DmlBufferTensorDesc* tensor, UINT64 bytes)
{
    if (!tensor || tensor->dataType != DML_TENSOR_DATA_TYPE_FLOAT32 ||
        tensor->totalTensorSizeInBytes != bytes)
        throw std::runtime_error("Incompatible ZipDepth DirectML input or output tensor");
}
}

struct ZipDepthNative::Impl {
    Library library;
    ComPtr<IDMLDevice1> dml;
    ComPtr<IDMLCommandRecorder> recorder;
    ComPtr<IDMLCompiledOperator> graph;
    ComPtr<ID3D12Resource> persistent;
    ComPtr<ID3D12Resource> temporary;
    Bindings bindings;
    bool fp16 = false;

    Impl(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* image,
        ID3D12Resource* depth, const std::filesystem::path& model, const std::filesystem::path& directml)
        : library(directml)
    {
        const auto create = reinterpret_cast<decltype(&DMLCreateDevice)>(
            GetProcAddress(library.handle, "DMLCreateDevice"));
        if (!create) throw std::runtime_error("DMLCreateDevice export missing");
        Check(create(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(dml.GetAddressOf())));
        Check(dml->CreateCommandRecorder(IID_PPV_ARGS(recorder.GetAddressOf())));

        const auto pack = ZipDepthPack::ReadFile(model);
        const auto entries = ZipDepthPack::Read(pack);
        const auto serialized = entries.at(ZipDepthPack::GraphFile);
        flatbuffers::Verifier verifier(serialized.data(), serialized.size());
        if (!dml::ir::VerifyDmlGraphDescBuffer(verifier))
            throw std::runtime_error("Invalid DirectML graph");
        const auto* root = dml::ir::GetDmlGraphDesc(serialized.data());
        if (!root->nodes() || !root->graphInputNames() || !root->graphOutputNames() ||
            root->graphOutputNames()->size() != 1 || root->graphOutputNames()->Get(0)->str() != "depth")
            throw std::runtime_error("Expected ZipDepth depth output");
        std::unordered_map<std::uint32_t, std::uint32_t> imageSlots;
        for (std::uint32_t i = 0; i < root->graphInputNames()->size(); ++i) {
            const auto name = root->graphInputNames()->Get(i)->string_view();
            if (name.empty()) continue; // ORT may pad this vector when one input has several edges.
            if (name != "image" || !imageSlots.empty()) throw std::runtime_error("Expected one ZipDepth image input");
            imageSlots.emplace(i, 0);
        }
        std::vector<std::unique_ptr<std::byte[]>> constantData;
        const auto decoded = DeserializeDmlGraph(serialized.data(), constantData);
        if (imageSlots.empty() || decoded.InputEdges.empty() || decoded.OutputEdges.size() != 1)
            throw std::runtime_error("Invalid ZipDepth graph edges");

        for (const auto& edge : decoded.InputEdges) {
            if (!imageSlots.contains(edge.GraphInputIndex)) throw std::runtime_error("Invalid ZipDepth input slot");
            const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.ToNodeIndex).Desc);
            CheckTensor(op.GetInputTensors().at(edge.ToNodeInputIndex), 3 * TensorBytes);
        }
        const auto& out = decoded.OutputEdges.front();
        const auto& outputOp = std::get<AbstractOperatorDesc>(decoded.Nodes.at(out.FromNodeIndex).Desc);
        CheckTensor(outputOp.GetOutputTensors().at(out.FromNodeOutputIndex), TensorBytes);

        for (const auto& node : decoded.Nodes) {
            if (const auto* op = std::get_if<AbstractOperatorDesc>(&node.Desc)) {
                for (const auto* tensor : op->GetOutputTensors())
                    fp16 |= tensor && tensor->dataType == DML_TENSOR_DATA_TYPE_FLOAT16;
            }
        }

        std::unordered_map<std::string_view, std::uint32_t> slots;
        std::vector<std::span<const std::uint8_t>> weights;
        for (const auto& edge : decoded.IntermediateEdges) {
            const auto& node = decoded.Nodes.at(edge.FromNodeIndex);
            const auto* constant = std::get_if<DmlSerializedGraphNodeConstantVariant>(&node.Desc);
            if (!constant) continue;
            const auto* external = std::get_if<ConstantName>(constant);
            if (!external) continue;
            const auto name = external->name + ".bin";
            const auto weight = entries.find(name);
            if (name == ZipDepthPack::GraphFile || weight == entries.end() || weight->second.size() % 4)
                throw std::runtime_error("Missing or unaligned ZipDepth graph weight");
            const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.ToNodeIndex).Desc);
            const auto* tensor = op.GetInputTensors().at(edge.ToNodeInputIndex);
            if (!tensor || tensor->totalTensorSizeInBytes > weight->second.size())
                throw std::runtime_error("ZipDepth graph weight is shorter than its tensor");
            const auto [slot, inserted] = slots.emplace(node.Name, static_cast<UINT>(weights.size() + 1));
            if (inserted) {
                weights.push_back(weight->second);
            } else if (weights.at(slot->second - 1).data() != weight->second.data()) {
                throw std::runtime_error("Conflicting ZipDepth graph weight binding");
            }
        }

        BucketAllocator allocator;
        DML_GRAPH_DESC desc{};
        std::vector<ComPtr<IDMLOperator>> operators;
        std::vector<DML_GRAPH_NODE_DESC> nodes;
        std::vector<DML_GRAPH_EDGE_DESC> inputs, outputs, intermediate;
        ConvertGraphDesc(decoded, 1, 1, dml.Get(), allocator, &imageSlots, &slots,
            desc, operators, nodes, inputs, outputs, intermediate);
        if (desc.InputCount != weights.size() + 1)
            throw std::runtime_error("Invalid ZipDepth weight bindings");
        Check(dml->CompileGraph(&desc, DML_EXECUTION_FLAG_NONE, IID_PPV_ARGS(graph.GetAddressOf())));
        Initialize(device, queue, weights);
        Bind(device, image, depth, desc.InputCount);
    }

    void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
        const std::vector<std::span<const std::uint8_t>>& weights)
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        const auto type = queue->GetDesc().Type;
        Check(device->CreateCommandAllocator(type, IID_PPV_ARGS(allocator.GetAddressOf())));
        Check(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf())));
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())));
        const std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
        if (!event) Check(HRESULT_FROM_WIN32(GetLastError()));

        std::vector<ComPtr<ID3D12Resource>> uploads, buffers;
        std::vector<DML_BUFFER_BINDING> initialInputs(weights.size() + 1);
        for (std::size_t i = 0; i < weights.size(); ++i) {
            const auto bytes = weights[i];
            auto upload = Buffer(device, bytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto weight = Buffer(device, bytes.size(), D3D12_HEAP_TYPE_DEFAULT, CopyDest);
            const D3D12_RANGE empty{0, 0}, written{0, bytes.size()};
            void* mapped = nullptr;
            Check(upload->Map(0, &empty, &mapped));
            std::memcpy(mapped, bytes.data(), bytes.size());
            upload->Unmap(0, &written);
            list->CopyBufferRegion(weight.Get(), 0, upload.Get(), 0, bytes.size());
            D3D12_RESOURCE_BARRIER transition{};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition = {weight.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, CopyDest, Uav};
            list->ResourceBarrier(1, &transition);
            initialInputs[i + 1] = {weight.Get(), 0, bytes.size()};
            uploads.push_back(std::move(upload));
            buffers.push_back(std::move(weight));
        }

        ComPtr<IDMLOperatorInitializer> initializer;
        IDMLCompiledOperator* graphs[]{graph.Get()};
        Check(dml->CreateOperatorInitializer(1, graphs, IID_PPV_ARGS(initializer.GetAddressOf())));
        const auto init = initializer->GetBindingProperties();
        const auto props = graph->GetBindingProperties();
        auto scratch = Buffer(device, init.TemporaryResourceSize, D3D12_HEAP_TYPE_DEFAULT, Uav);
        persistent = Buffer(device, props.PersistentResourceSize, D3D12_HEAP_TYPE_DEFAULT, Uav);
        const DML_BUFFER_ARRAY_BINDING input{static_cast<UINT>(initialInputs.size()), initialInputs.data()};
        const DML_BINDING_DESC weightBindings{DML_BINDING_TYPE_BUFFER_ARRAY, &input};
        const DML_BUFFER_BINDING scratchBuffer{scratch.Get(), 0, init.TemporaryResourceSize};
        const DML_BUFFER_BINDING persistentBuffer{persistent.Get(), 0, props.PersistentResourceSize};
        const auto scratchBinding = Binding(scratchBuffer), outputBinding = Binding(persistentBuffer);
        Bindings initializerBindings;
        initializerBindings.Create(device, dml.Get(), initializer.Get());
        initializerBindings.table->BindInputs(1, &weightBindings);
        initializerBindings.table->BindOutputs(1, &outputBinding);
        if (scratch) initializerBindings.table->BindTemporaryResource(&scratchBinding);
        initializerBindings.Record(list.Get(), recorder.Get(), initializer.Get());
        if (persistent) Barrier(list.Get(), persistent.Get());

        Check(list->Close());
        Check(fence->SetEventOnCompletion(1, event.get()));
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), 1));
        if (WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0) Check(E_FAIL);
        Check(device->GetDeviceRemovedReason());
    }

    void Bind(ID3D12Device* device, ID3D12Resource* image, ID3D12Resource* depth, UINT slots)
    {
        const auto props = graph->GetBindingProperties();
        temporary = Buffer(device, props.TemporaryResourceSize, D3D12_HEAP_TYPE_DEFAULT, Uav);
        bindings.Create(device, dml.Get(), graph.Get());
        const DML_BUFFER_BINDING imageBuffer{image, 0, 3 * TensorBytes}, depthBuffer{depth, 0, TensorBytes};
        const DML_BUFFER_BINDING scratchBuffer{temporary.Get(), 0, props.TemporaryResourceSize};
        const DML_BUFFER_BINDING persistentBuffer{persistent.Get(), 0, props.PersistentResourceSize};
        std::vector<DML_BINDING_DESC> inputs(slots, {DML_BINDING_TYPE_NONE, nullptr});
        inputs.front() = Binding(imageBuffer);
        const auto output = Binding(depthBuffer), scratch = Binding(scratchBuffer), state = Binding(persistentBuffer);
        bindings.table->BindInputs(slots, inputs.data());
        bindings.table->BindOutputs(1, &output);
        if (temporary) bindings.table->BindTemporaryResource(&scratch);
        if (persistent) bindings.table->BindPersistentResource(&state);
        Check(device->GetDeviceRemovedReason());
    }
};

ZipDepthNative::ZipDepthNative(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* image, ID3D12Resource* depth, const std::filesystem::path& model,
    const std::filesystem::path& directml)
    : m_impl(std::make_unique<Impl>(device, queue, image, depth, model, directml))
{
}

ZipDepthNative::~ZipDepthNative() = default;

void ZipDepthNative::Record(ID3D12GraphicsCommandList* list) const
{
    m_impl->bindings.Record(list, m_impl->recorder.Get(), m_impl->graph.Get());
    if (m_impl->temporary) Barrier(list, m_impl->temporary.Get());
}

bool ZipDepthNative::UsesFp16() const { return m_impl->fp16; }
