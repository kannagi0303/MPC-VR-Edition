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
#include <array>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <memory>
#include <limits>
#include <stdexcept>

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
[[noreturn]] void Fail(HRESULT hr, const char* operation = nullptr)
{
    char text[96]{};
    std::snprintf(text, sizeof(text), "%s%sHRESULT 0x%08X",
        operation ? operation : "", operation ? " " : "", static_cast<unsigned>(hr));
    throw std::runtime_error(text);
}

void Check(HRESULT hr, const char* operation = nullptr)
{
    if (FAILED(hr)) Fail(hr, operation);
}
}

#include "DirectMLGraph/GraphHelpers.h"

namespace {
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
        throw std::runtime_error("DirectML resource exceeds the supported buffer size");
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
        throw std::runtime_error("Incompatible DirectML input or output tensor");
}
}

namespace {
struct NativeGraphExecution {
    Library library;
    ComPtr<IDMLDevice1> dml;
    ComPtr<IDMLCommandRecorder> recorder;
    ComPtr<IDMLCompiledOperator> graph;
    ComPtr<ID3D12Resource> persistent;
    ComPtr<ID3D12Resource> temporary;
    Bindings bindings;

    NativeGraphExecution(ID3D12Device* device, const std::filesystem::path& directml)
        : library(directml)
    {
        const auto create = reinterpret_cast<decltype(&DMLCreateDevice)>(
            GetProcAddress(library.handle, "DMLCreateDevice"));
        if (!create) throw std::runtime_error("DMLCreateDevice export missing");
        Check(create(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(dml.GetAddressOf())));
        Check(dml->CreateCommandRecorder(IID_PPV_ARGS(recorder.GetAddressOf())));
    }

    void Compile(const DmlSerializedGraphDesc& decoded, UINT inputCount, UINT outputCount,
        const std::unordered_map<std::uint32_t, std::uint32_t>* imageSlots,
        const std::unordered_map<std::string_view, std::uint32_t>* slots, UINT runtimeInputs = 1)
    {
        BucketAllocator allocator;
        DML_GRAPH_DESC desc{};
        std::vector<ComPtr<IDMLOperator>> operators;
        std::vector<DML_GRAPH_NODE_DESC> nodes;
        std::vector<DML_GRAPH_EDGE_DESC> inputs, outputs, intermediate;
        ConvertGraphDesc(decoded, runtimeInputs, outputCount, dml.Get(), allocator, imageSlots, slots,
            desc, operators, nodes, inputs, outputs, intermediate);
        if (desc.InputCount != inputCount || desc.OutputCount != outputCount)
            throw std::runtime_error("Invalid DirectML graph bindings");
        Check(dml->CompileGraph(&desc, DML_EXECUTION_FLAG_NONE, IID_PPV_ARGS(graph.GetAddressOf())), "CompileGraph");
    }

    void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
        const std::vector<std::span<const std::uint8_t>>& weights, UINT runtimeInputs = 1)
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
        std::vector<DML_BUFFER_BINDING> initialInputs(weights.size() + runtimeInputs);
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
            initialInputs[i + runtimeInputs] = {weight.Get(), 0, bytes.size()};
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

    void BindSet(ID3D12Device* device, Bindings& target,
        std::span<const DML_BUFFER_BINDING> runtime, std::span<const DML_BUFFER_BINDING> outputs, UINT slots)
    {
        const auto props = graph->GetBindingProperties();
        if (!temporary) temporary = Buffer(device, props.TemporaryResourceSize, D3D12_HEAP_TYPE_DEFAULT, Uav);
        target.Create(device, dml.Get(), graph.Get());
        const DML_BUFFER_BINDING scratchBuffer{temporary.Get(), 0, props.TemporaryResourceSize};
        const DML_BUFFER_BINDING persistentBuffer{persistent.Get(), 0, props.PersistentResourceSize};
        std::vector<DML_BINDING_DESC> inputs(slots, {DML_BINDING_TYPE_NONE, nullptr});
        if (runtime.size() > slots) throw std::runtime_error("Invalid runtime bindings");
        std::transform(runtime.begin(), runtime.end(), inputs.begin(), Binding);
        std::vector<DML_BINDING_DESC> outputBindings(outputs.size());
        std::transform(outputs.begin(), outputs.end(), outputBindings.begin(), Binding);
        const auto scratch = Binding(scratchBuffer), state = Binding(persistentBuffer);
        target.table->BindInputs(slots, inputs.data());
        target.table->BindOutputs(static_cast<UINT>(outputs.size()), outputBindings.data());
        if (temporary) target.table->BindTemporaryResource(&scratch);
        if (persistent) target.table->BindPersistentResource(&state);
        Check(device->GetDeviceRemovedReason());
    }

    template<std::size_t Count>
    void Bind(ID3D12Device* device, const DML_BUFFER_BINDING& image,
        const std::array<DML_BUFFER_BINDING, Count>& outputs, UINT slots)
    {
        BindSet(device, bindings, std::span(&image, 1), outputs, slots);
    }

    void Record(ID3D12GraphicsCommandList* list) const
    {
        bindings.Record(list, recorder.Get(), graph.Get());
        if (temporary) Barrier(list, temporary.Get());
    }
};
} // namespace

#include "DepthNative.h"
#include "DepthPack.h"
#include "DepthSplitPack.h"
#include "DepthStreamContract.h"

struct DepthNative::Impl : NativeGraphExecution {
    struct StateSlot {
        std::string name;
        UINT output = 0;
        UINT64 bytes = 0;
        DML_TENSOR_DATA_TYPE type = DML_TENSOR_DATA_TYPE_UNKNOWN;
        std::array<ComPtr<ID3D12Resource>, 2> buffers;
    };
    std::vector<StateSlot> states;
    Bindings alternate;
    ComPtr<ID3D12Resource> zeros;
    mutable depthstream::BankCycle cycle;

    Impl(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* image,
        ID3D12Resource* depth, std::span<const std::uint8_t> model, const std::filesystem::path& directml,
        std::span<const std::uint8_t> pool)
        : NativeGraphExecution(device, directml)
    {
        const auto TensorBytes = DepthPack::Pixels * sizeof(float);
        const auto entries = depthsplit::Open(model, pool);
        const auto serialized = entries.at(DepthPack::GraphFile);
        flatbuffers::Verifier verifier(serialized.data(), serialized.size());
        if (!dml::ir::VerifyDmlGraphDescBuffer(verifier))
            throw std::runtime_error("Invalid DirectML graph");
        const auto* root = dml::ir::GetDmlGraphDesc(serialized.data());
        if (!root->nodes() || !root->graphInputNames() || !root->graphOutputNames())
            throw std::runtime_error("Missing DirectML graph names");
        const auto* outputNames = root->graphOutputNames();
        const UINT outputCount = outputNames->size();
        if (outputCount < 2 || outputCount > depthstream::MaxStates + 1)
            throw std::runtime_error("Invalid depth output count");
        std::unordered_map<std::uint32_t, std::uint32_t> imageSlots;
        std::map<std::string, UINT, std::less<>> namedInputs, namedOutputs;
        for (UINT i = 0; i < outputCount; ++i) {
            const auto name = outputNames->Get(i)->str();
            if (name.empty() || !namedOutputs.emplace(name, i).second)
                throw std::runtime_error("Invalid depth output name");
        }
        for (UINT i = 0; i < root->graphInputNames()->size(); ++i) {
            const auto name = root->graphInputNames()->Get(i)->str();
            if (name.empty()) continue; // ORT may pad names for repeated edges.
            if (!namedInputs.emplace(name, i).second)
                throw std::runtime_error("Duplicate depth input name");
        }
        if (!namedInputs.contains("image") || !namedOutputs.contains("depth") ||
            namedInputs.size() != outputCount)
            throw std::runtime_error("Invalid depth image/state interface");
        imageSlots.emplace(namedInputs.at("image"), 0);
        const UINT depthOutput = namedOutputs.at("depth");
        for (const auto& [name, index] : namedInputs) {
            if (name == "image") continue;
            const auto present = depthstream::Present(name);
            if (!namedOutputs.contains(present)) throw std::runtime_error("Unpaired depth state");
            imageSlots.emplace(index, static_cast<UINT>(states.size() + 1));
            states.push_back({name, namedOutputs.at(present)});
        }
        const UINT runtimeInputs = static_cast<UINT>(states.size() + 1);
        std::vector<std::unique_ptr<std::byte[]>> constantData;
        const auto decoded = DeserializeDmlGraph(serialized.data(), constantData);
        if (decoded.InputEdges.empty() || decoded.OutputEdges.size() != outputCount)
            throw std::runtime_error("Invalid depth graph edges");
        std::vector<bool> seenOutputs(outputCount), seenInputs(runtimeInputs);
        UINT64 totalState = 0, maxState = 0;
        for (const auto& out : decoded.OutputEdges) {
            if (out.GraphOutputIndex >= outputCount || seenOutputs[out.GraphOutputIndex])
                throw std::runtime_error("Invalid depth output edge");
            seenOutputs[out.GraphOutputIndex] = true;
            const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(out.FromNodeIndex).Desc);
            const auto* tensor = op.GetOutputTensors().at(out.FromNodeOutputIndex);
            if (out.GraphOutputIndex == depthOutput) { CheckTensor(tensor, TensorBytes); continue; }
            auto it = std::find_if(states.begin(), states.end(), [&](const auto& st) { return st.output == out.GraphOutputIndex; });
            if (it == states.end() || !tensor || (tensor->dataType != DML_TENSOR_DATA_TYPE_FLOAT32 &&
                tensor->dataType != DML_TENSOR_DATA_TYPE_FLOAT16) || !tensor->totalTensorSizeInBytes ||
                tensor->totalTensorSizeInBytes % 4 || tensor->totalTensorSizeInBytes > depthstream::MaxStateBytes)
                throw std::runtime_error("Invalid depth state output tensor");
            it->bytes = tensor->totalTensorSizeInBytes;
            it->type = tensor->dataType;
            totalState += it->bytes;
            maxState = std::max(maxState, it->bytes);
            if (totalState > depthstream::MaxStateBytes) throw std::runtime_error("Depth state exceeds limit");
        }
        for (const auto& edge : decoded.InputEdges) {
            const auto slot = imageSlots.at(edge.GraphInputIndex);
            seenInputs[slot] = true;
            const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.ToNodeIndex).Desc);
            const auto* tensor = op.GetInputTensors().at(edge.ToNodeInputIndex);
            if (slot == 0) CheckTensor(tensor, 3 * TensorBytes);
            else {
                const auto& st = states.at(slot - 1);
                if (!tensor || tensor->dataType != st.type || !tensor->totalTensorSizeInBytes ||
                    tensor->totalTensorSizeInBytes > st.bytes || (tensor->flags & DML_TENSOR_FLAG_OWNED_BY_DML))
                    throw std::runtime_error("Incompatible depth state input tensor");
            }
        }
        if (std::find(seenInputs.begin(), seenInputs.end(), false) != seenInputs.end())
            throw std::runtime_error("Unused depth state input");

        bool fp16 = false;
        for (const auto& node : decoded.Nodes) {
            if (const auto* op = std::get_if<AbstractOperatorDesc>(&node.Desc)) {
                for (const auto* tensor : op->GetOutputTensors())
                    fp16 |= tensor && tensor->dataType == DML_TENSOR_DATA_TYPE_FLOAT16;
            }
        }

        if (!fp16) throw std::runtime_error("Expected mixed FP16 depth operators");

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
            if (name == DepthPack::GraphFile || weight == entries.end() || weight->second.size() % 4)
                throw std::runtime_error("Missing or unaligned depth graph weight");
            const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.ToNodeIndex).Desc);
            const auto* tensor = op.GetInputTensors().at(edge.ToNodeInputIndex);
            if (!tensor || tensor->totalTensorSizeInBytes > weight->second.size())
                throw std::runtime_error("depth graph weight is shorter than its tensor");
            const auto [slot, inserted] = slots.emplace(node.Name, static_cast<UINT>(weights.size() + runtimeInputs));
            if (inserted) {
                weights.push_back(weight->second);
            } else if (weights.at(slot->second - runtimeInputs).data() != weight->second.data()) {
                throw std::runtime_error("Conflicting depth graph weight binding");
            }
        }

        const auto inputCount = static_cast<UINT>(weights.size() + runtimeInputs);
        Compile(decoded, inputCount, outputCount, &imageSlots, &slots, runtimeInputs);
        Initialize(device, queue, weights, runtimeInputs);
        if (!states.empty()) {
            zeros = Buffer(device, maxState, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            const D3D12_RANGE empty{0, 0}, written{0, static_cast<SIZE_T>(maxState)};
            void* data = nullptr;
            Check(zeros->Map(0, &empty, &data));
            std::memset(data, 0, static_cast<size_t>(maxState));
            zeros->Unmap(0, &written);
            for (auto& st : states)
                for (auto& buffer : st.buffers) buffer = Buffer(device, st.bytes, D3D12_HEAP_TYPE_DEFAULT, Uav);
        }
        for (unsigned i = 0; i < (states.empty() ? 1u : 2u); ++i) {
            std::vector<DML_BUFFER_BINDING> ins{{image, 0, 3 * TensorBytes}};
            std::vector<DML_BUFFER_BINDING> outs(outputCount);
            outs[depthOutput] = {depth, 0, TensorBytes};
            for (const auto& st : states) {
                ins.push_back({st.buffers[i].Get(), 0, st.bytes});
                outs[st.output] = {st.buffers[1-i].Get(), 0, st.bytes};
            }
            BindSet(device, i ? alternate : bindings, ins, outs, inputCount);
        }
    }

    void RecordState(ID3D12GraphicsCommandList* list, bool reset) const
    {
        if (cycle.Begin(reset)) {
            for (const auto& st : states) {
                auto* buffer = st.buffers[cycle.input].Get();
                D3D12_RESOURCE_BARRIER transition{};
                transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                transition.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, Uav, CopyDest};
                list->ResourceBarrier(1, &transition);
                list->CopyBufferRegion(buffer, 0, zeros.Get(), 0, st.bytes);
                std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
                list->ResourceBarrier(1, &transition);
            }
        }
        (cycle.input ? alternate : bindings).Record(list, recorder.Get(), graph.Get());
        // Order all cache reads and writes before the next dispatch swaps the banks.
        Barrier(list, nullptr);
        cycle.Advance();
    }

};

DepthNative::DepthNative(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* image, ID3D12Resource* depth, std::span<const std::uint8_t> model,
    const std::filesystem::path& directml, std::span<const std::uint8_t> weights)
    : m_impl(std::make_unique<Impl>(device, queue, image, depth, model, directml, weights))
{
}

DepthNative::~DepthNative() = default;

void DepthNative::Record(ID3D12GraphicsCommandList* list, bool resetState) const
{
    m_impl->RecordState(list, resetState);
}

// The standalone depth converter shares this file without the renderer.
#ifdef MPCVR_ENABLE_SVDLUT
#include "../DLSS/SvdLutModel.inc"
#endif
