// SPDX-License-Identifier: GPL-3.0-or-later
// Included by ZipDepthNative.cpp so both native models use the same graph helpers.
#include "SvdLutModel.h"
#include "../Edition/SvdLutLutRegularity.h"

#include <d3d12.h>
#include <dxgi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3d12.lib")

namespace svdlut {
namespace {

constexpr auto QueueType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
constexpr UINT64 ImageBytes = sizeof(Image);
constexpr std::array<UINT64, 4> OutputBytes{
    9 * LutSize * LutSize * sizeof(float), 12 * sizeof(float),
    18 * GridSize * GridSize * sizeof(float), 24 * sizeof(float)
};
constexpr std::array<std::string_view, 4> OutputNames{"lut_planes", "lut_mix", "grid_planes", "grid_mix"};
constexpr UINT64 ReadbackBytes = OutputBytes[0] + OutputBytes[1] + OutputBytes[2] + OutputBytes[3];

// The graph stores two outputs flattened intentionally; validate storage shape,
// not just the same byte count (e.g. a transposed RGB plane is not compatible).
constexpr std::array<uint32_t, 4> InputShape{1, 3, InputSize, InputSize};
constexpr std::array<std::array<uint32_t, 4>, 4> OutputShapes{{
    {1, 9, LutSize, LutSize}, {1, 1, 1, 12},
    {1, 1, 1, 18 * GridSize * GridSize}, {1, 1, 1, 24}
}};
void CheckSvdLutTensor(const DmlBufferTensorDesc* tensor, UINT64 bytes,
    const std::array<uint32_t, 4>& shape)
{
    CheckTensor(tensor, bytes);
    if (tensor->sizes.size() != shape.size() ||
        !std::equal(shape.begin(), shape.end(), tensor->sizes.begin()))
        throw std::runtime_error("Incompatible SVDLUT tensor shape");
    if (tensor->strides) {
        const auto& strides = *tensor->strides;
        if (strides.size() != shape.size()) throw std::runtime_error("Incompatible SVDLUT strides");
        uint32_t stride = 1;
        for (size_t i = shape.size(); i-- > 0;) {
            // Singleton axes never contribute to the address.
            if (shape[i] > 1 && strides[i] != stride)
                throw std::runtime_error("Non-contiguous SVDLUT tensor");
            stride *= shape[i];
        }
    }
}

class StartupTrace {
    std::unique_ptr<void, decltype(&CloseHandle)> m_file{nullptr, CloseHandle};
    const char* m_stage = "Model file";

public:
    StartupTrace() noexcept
    {
        wchar_t path[MAX_PATH]{};
        constexpr wchar_t name[] = L"MPCVR-SVDLUT-startup.log";
        const auto size = GetTempPathW(static_cast<DWORD>(std::size(path)), path);
        if (!size || size > std::size(path) - std::size(name)) return;
        std::copy(std::begin(name), std::end(name), path + size);
        const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) m_file.reset(file);
    }

    void Write(std::string_view text) const noexcept
    {
        if (!m_file) return;
        DWORD written;
        WriteFile(m_file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        WriteFile(m_file.get(), "\r\n", 2, &written, nullptr);
    }

    void Mark(const char* stage) noexcept
    {
        m_stage = stage;
        Write(stage);
    }

    const char* Stage() const { return m_stage; }
    void Close() { m_file.reset(); }
};

std::vector<std::uint8_t> ReadModel(const std::filesystem::path& directory)
{
    std::ifstream file(directory / L"svdlut-fivek-srgb.dmlgraph", std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= std::streampos(0) || static_cast<std::uint64_t>(size) > 8 * 1024 * 1024) throw std::runtime_error("File missing, empty or oversized");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Unable to read SVDLUT model");
    return bytes;
}

void MapModelWeights(DmlSerializedGraphDesc& decoded,
    std::unordered_map<std::string_view, std::uint32_t>& slots,
    std::vector<std::span<const std::uint8_t>>& weights)
{
    for (auto& node : decoded.Nodes) {
        auto* constant = std::get_if<DmlSerializedGraphNodeConstantVariant>(&node.Desc);
        if (!constant) continue;
        const auto* raw = std::get_if<ConstantData>(constant);
        if (!raw || !raw->data || !raw->dataSize || raw->dataSize % sizeof(float))
            throw std::runtime_error("Unexpected SVDLUT weight");
        const auto slot = static_cast<UINT>(weights.size() + 1);
        if (!slots.emplace(node.Name, slot).second) throw std::runtime_error("Duplicate SVDLUT weight");
        weights.emplace_back(reinterpret_cast<const std::uint8_t*>(raw->data), static_cast<size_t>(raw->dataSize));
        // Use the same initializer-owned inputs as ZipDepth. The bytes stay
        // in the model file, but CompileGraph never sees inline constant nodes.
        *constant = ConstantName{node.Name};
    }

    std::vector<bool> used(weights.size());
    for (const auto& edge : decoded.IntermediateEdges) {
        const auto& node = decoded.Nodes.at(edge.FromNodeIndex);
        if (!std::holds_alternative<DmlSerializedGraphNodeConstantVariant>(node.Desc)) continue;
        if (edge.FromNodeOutputIndex != 0) throw std::runtime_error("Invalid SVDLUT weight edge");
        const auto index = slots.at(node.Name) - 1;
        auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.ToNodeIndex).Desc);
        auto* tensor = op.GetInputTensors().at(edge.ToNodeInputIndex);
        CheckTensor(tensor, weights[index].size());
        tensor->flags = DML_TENSOR_FLAG_OWNED_BY_DML;
        used[index] = true;
    }
    if (weights.empty() || std::find(used.begin(), used.end(), false) != used.end())
        throw std::runtime_error("Missing or unused SVDLUT weights");
}

UINT InitializeModel(NativeGraphExecution& graph, ID3D12Device* device, ID3D12CommandQueue* queue,
    std::span<const std::uint8_t> bytes, StartupTrace& trace)
{
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (!dml::ir::VerifyDmlGraphDescBuffer(verifier)) throw std::runtime_error("Invalid SVDLUT graph");
    const auto* root = dml::ir::GetDmlGraphDesc(bytes.data());
    const auto* inputs = root->graphInputNames();
    const auto* outputs = root->graphOutputNames();
    if (!root->nodes() || !inputs || inputs->size() != 1 || inputs->Get(0)->string_view() != "image" ||
        !outputs || outputs->size() != OutputNames.size())
        throw std::runtime_error("Invalid SVDLUT graph signature");
    for (UINT i = 0; i < OutputNames.size(); ++i) {
        if (outputs->Get(i)->string_view() != OutputNames[i]) throw std::runtime_error("Unexpected SVDLUT output");
    }

    std::vector<std::unique_ptr<std::byte[]>> constantData;
    auto decoded = DeserializeDmlGraph(bytes.data(), constantData);
    if (decoded.InputEdges.empty() || decoded.OutputEdges.size() != OutputNames.size())
        throw std::runtime_error("Invalid SVDLUT graph edges");
    for (const auto& edge : decoded.InputEdges) {
        if (edge.GraphInputIndex != 0) throw std::runtime_error("Unexpected SVDLUT input");
        const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.ToNodeIndex).Desc);
        CheckSvdLutTensor(op.GetInputTensors().at(edge.ToNodeInputIndex), ImageBytes, InputShape);
    }
    std::array<bool, OutputNames.size()> seen{};
    for (const auto& edge : decoded.OutputEdges) {
        if (seen.at(edge.GraphOutputIndex)) throw std::runtime_error("Duplicate SVDLUT output");
        seen[edge.GraphOutputIndex] = true;
        const auto& op = std::get<AbstractOperatorDesc>(decoded.Nodes.at(edge.FromNodeIndex).Desc);
        CheckSvdLutTensor(op.GetOutputTensors().at(edge.FromNodeOutputIndex), OutputBytes[edge.GraphOutputIndex],
            OutputShapes[edge.GraphOutputIndex]);
    }
    trace.Mark("Map initializer weights");
    std::unordered_map<std::string_view, std::uint32_t> slots;
    std::vector<std::span<const std::uint8_t>> weights;
    MapModelWeights(decoded, slots, weights);
    const auto inputCount = static_cast<UINT>(weights.size() + 1);
    trace.Mark("Graph build and CompileGraph");
    graph.Compile(decoded, inputCount, static_cast<UINT>(OutputNames.size()), nullptr, &slots);
    trace.Mark("DirectML initialize");
    graph.Initialize(device, queue, weights);
    return inputCount;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

std::unique_ptr<Parameters> PackParameters(const float* lut, const float* lutMix,
    const float* grid, const float* gridMix)
{
    constexpr unsigned LutArea = LutSize * LutSize;
    constexpr unsigned GridArea = GridSize * GridSize;
    static_assert(LutSize == 2 * GridSize - 1);
    auto packed = std::make_unique<Parameters>();

    // The published FiveK kernel integer-divides x and y before converting
    // them to float. The grid only uses x = 1 on the last column and y = 1
    // on the last row. Fold each edge combination into the three LUT planes.
    for (unsigned variant = 0; variant < LutLayers / 3; ++variant) {
        const unsigned gx = (variant & 1) ? GridSize - 1 : 0;
        const unsigned gy = (variant & 2) ? GridSize - 1 : 0;
        for (unsigned channel = 0; channel < 3; ++channel) {
            float bias = lutMix[channel * 4 + 3];
            std::array<float, GridSize> curve{};
            for (unsigned layer = 0; layer < 2; ++layer) {
                const unsigned group = channel * 2 + layer;
                const auto* planes = grid + group * 3 * GridArea;
                const auto* mix = gridMix + (group / 3) * 12;
                const auto* weights = mix + (group % 3) * 3;
                bias += mix[9 + group % 3] + planes[gy * GridSize + gx] * weights[0];
                for (unsigned color = 0; color < GridSize; ++color) {
                    const unsigned row = color * GridSize;
                    curve[color] += planes[GridArea + row + gx] * weights[1] +
                        planes[2 * GridArea + row + gy] * weights[2];
                }
            }

            // RG holds the R/G correction and RB holds B. Along either
            // axis the 33-node LUT aligns with the knots of the 17-node curve.
            const unsigned correctionPlane = channel == 2 ? 1 : 0;
            for (unsigned plane = 0; plane < 3; ++plane) {
                const auto* source = lut + (channel * 3 + plane) * LutArea;
                auto* destination = packed->lut.data() + (variant * 3 + plane) * LutArea;
                const float weight = lutMix[channel * 4 + plane];
                for (unsigned i = 0; i < LutArea; ++i) {
                    float value = source[i] * weight;
                    if (plane == correctionPlane) {
                        const unsigned color = channel == 0 ? i % LutSize : i / LutSize;
                        const float offset = (curve[color / 2] + curve[(color + 1) / 2]) * 0.5f;
                        value += bias + offset;
                    }
                    destination[i][channel] = value;
                }
            }
        }
    }
    return packed;
}

ComPtr<ID3D11ShaderResourceView> CreateGradeView(ID3D11Device* device, const Parameters& grade)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = LutSize;
    desc.ArraySize = LutLayers;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    constexpr UINT area = LutSize * LutSize;
    constexpr UINT pitch = LutSize * sizeof(Parameters::Texel);
    std::array<D3D11_SUBRESOURCE_DATA, LutLayers> data{};
    for (UINT layer = 0; layer < LutLayers; ++layer) {
        data[layer] = {grade.lut.data() + layer * area, pitch, 0};
    }
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    Check(device->CreateTexture2D(&desc, data.data(), texture.GetAddressOf()), "SVDLUT texture");
    Check(device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()), "SVDLUT texture view");
    return view;
}

class Inference {
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence> m_fence;
    std::unique_ptr<void, decltype(&CloseHandle)> m_event{nullptr, CloseHandle};
    ComPtr<ID3D12Resource> m_image, m_upload, m_readback;
    std::array<ComPtr<ID3D12Resource>, OutputNames.size()> m_outputs;
    std::unique_ptr<NativeGraphExecution> m_graph;
    UINT64 m_fenceValue = 0;

public:
    Inference(IDXGIAdapter* adapter, const std::filesystem::path& directory, StartupTrace& trace)
    {
        const auto bytes = ReadModel(directory);
        trace.Mark("D3D12 setup");
        Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_device.GetAddressOf())));
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = QueueType;
        Check(m_device->CreateCommandQueue(&queue, IID_PPV_ARGS(m_queue.GetAddressOf())));
        Check(m_device->CreateCommandAllocator(QueueType, IID_PPV_ARGS(m_allocator.GetAddressOf())));
        Check(m_device->CreateCommandList(0, QueueType, m_allocator.Get(), nullptr, IID_PPV_ARGS(m_list.GetAddressOf())));
        Check(m_list->Close());
        Check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
        m_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!m_event) Check(HRESULT_FROM_WIN32(GetLastError()));

        m_image = Buffer(m_device.Get(), ImageBytes, D3D12_HEAP_TYPE_DEFAULT, CopyDest);
        m_upload = Buffer(m_device.Get(), ImageBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        m_readback = Buffer(m_device.Get(), ReadbackBytes, D3D12_HEAP_TYPE_READBACK, CopyDest);
        std::array<DML_BUFFER_BINDING, OutputNames.size()> outputs{};
        for (UINT i = 0; i < outputs.size(); ++i) {
            m_outputs[i] = Buffer(m_device.Get(), OutputBytes[i], D3D12_HEAP_TYPE_DEFAULT, Uav);
            outputs[i] = {m_outputs[i].Get(), 0, OutputBytes[i]};
        }

        trace.Mark("DirectML load");
        m_graph = std::make_unique<NativeGraphExecution>(m_device.Get(), directory / L"DirectML.dll");
        trace.Mark("Graph decode");
        const auto inputCount = InitializeModel(*m_graph, m_device.Get(), m_queue.Get(), bytes, trace);
        trace.Mark("DirectML bindings");
        m_graph->Bind(m_device.Get(), {m_image.Get(), 0, ImageBytes}, outputs, inputCount);
    }

    std::unique_ptr<Parameters> Evaluate(const Image& image)
    {
        Check(m_allocator->Reset());
        Check(m_list->Reset(m_allocator.Get(), nullptr));
        const D3D12_RANGE empty{0, 0}, inputRange{0, ImageBytes}, outputRange{0, ReadbackBytes};
        void* data = nullptr;
        Check(m_upload->Map(0, &empty, &data));
        std::memcpy(data, image.data(), ImageBytes);
        m_upload->Unmap(0, &inputRange);
        m_list->CopyBufferRegion(m_image.Get(), 0, m_upload.Get(), 0, ImageBytes);
        Transition(m_list.Get(), m_image.Get(), CopyDest, Uav);
        m_graph->Record(m_list.Get());
        Transition(m_list.Get(), m_image.Get(), Uav, CopyDest);
        UINT64 offset = 0;
        for (UINT i = 0; i < m_outputs.size(); ++i) {
            auto* output = m_outputs[i].Get();
            Transition(m_list.Get(), output, Uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
            m_list->CopyBufferRegion(m_readback.Get(), offset, output, 0, OutputBytes[i]);
            Transition(m_list.Get(), output, D3D12_RESOURCE_STATE_COPY_SOURCE, Uav);
            offset += OutputBytes[i];
        }
        Check(m_list->Close());
        const auto value = ++m_fenceValue;
        Check(m_fence->SetEventOnCompletion(value, m_event.get()));
        ID3D12CommandList* lists[]{m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        Check(m_queue->Signal(m_fence.Get(), value));
        if (WaitForSingleObject(m_event.get(), INFINITE) != WAIT_OBJECT_0) Check(E_FAIL);
        Check(m_device->GetDeviceRemovedReason());

        Check(m_readback->Map(0, &outputRange, &data));
        std::array<float, ReadbackBytes / sizeof(float)> values;
        std::memcpy(values.data(), data, ReadbackBytes);
        m_readback->Unmap(0, &empty);
        if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }))
            throw std::runtime_error("SVDLUT returned non-finite parameters");
        const float* lut = values.data();
        const float* lutMix = lut + OutputBytes[0] / sizeof(float);
        const float* grid = lutMix + OutputBytes[1] / sizeof(float);
        const float* gridMix = grid + OutputBytes[2] / sizeof(float);
        return PackParameters(lut, lutMix, grid, gridMix);
    }
};

bool PinRenderer()
{
    static const bool pinned = [] {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&PinRenderer), &module) != FALSE;
    }();
    return pinned;
}

 } // namespace

ComPtr<ID3D11ShaderResourceView> CreateParametersView(ID3D11Device* device, const Parameters& parameters)
{
    if (!device) return {};
    try { return CreateGradeView(device, parameters); }
    catch (const std::exception&) { return {}; }
}

struct ModelWorker::Impl {
    std::atomic<State> state = State::Loading;
    std::array<char, 160> failure{};
    std::atomic_bool finished = false;
    std::thread thread;
    std::mutex lock;
    std::condition_variable wake;
    std::unique_ptr<Image> input;
    ComPtr<ID3D11PixelShader> shader;
    ComPtr<ID3D11ShaderResourceView> result;
    std::shared_ptr<const Parameters> resultParameters;
    uint64_t generation = 0;
    bool running = false;
    bool stopping = false;
    bool regularizeB = false;

    void RequestStop()
    {
        {
            std::lock_guard guard(lock);
            stopping = true;
            input.reset();
        }
        wake.notify_all();
    }

    ~Impl()
    {
        RequestStop();
        if (thread.joinable()) thread.join();
    }

    void Run(IDXGIAdapter* adapter, ID3D11Device* renderDevice, const std::filesystem::path& directory)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        StartupTrace trace;
        trace.Mark("Model file");
        try {
            Inference model(adapter, directory, trace);
            trace.Mark("SVDLUT pixel shader");
            Check(CreateGradeShader(renderDevice, shader.GetAddressOf()));
            trace.Mark("Waiting for video thumbnail");
            bool first = true;
            state.store(State::Ready, std::memory_order_release);
            for (;;) {
                std::unique_lock guard(lock);
                wake.wait(guard, [&] { return stopping || input; });
                if (stopping) break;
                auto image = std::move(input);
                const auto submittedGeneration = generation;
                running = true;
                guard.unlock();
                if (first) trace.Mark("First DirectML dispatch");
                auto parameters = model.Evaluate(*image);
                if (regularizeB && parameters) {
                    // Regularity is optional: allocation failure must not disable
                    // SVDLUT playback or alter its existing fallback path.
                    try { regularity::Regularize(*parameters); }
                    catch (const std::bad_alloc&) {}
                }
                guard.lock();
                const bool current = !stopping && generation == submittedGeneration;
                guard.unlock();
                ComPtr<ID3D11ShaderResourceView> view;
                if (current) view = CreateGradeView(renderDevice, *parameters);
                if (first) {
                    trace.Mark("First DirectML dispatch complete");
                    trace.Close();
                    trace.Mark("DirectML inference");
                    first = false;
                }
                guard.lock();
                if (!stopping && generation == submittedGeneration) {
                    result = std::move(view);
                    resultParameters = std::move(parameters);
                }
                running = false;
                wake.notify_all();
            }
        } catch (const std::exception& error) {
            OutputDebugStringA("SVDLUT DirectML: ");
            std::snprintf(failure.data(), failure.size(), "%s: %s", trace.Stage(), error.what());
            trace.Write(failure.data());
            OutputDebugStringA(failure.data());
            OutputDebugStringA("\n");
            {
                std::lock_guard guard(lock);
                state.store(State::Failed, std::memory_order_release);
            }
            wake.notify_all();
        }
        finished.store(true, std::memory_order_release);
    }

    static std::mutex retiredLock;
    static std::vector<std::unique_ptr<Impl>> retired;
};

std::mutex ModelWorker::Impl::retiredLock;
std::vector<std::unique_ptr<ModelWorker::Impl>> ModelWorker::Impl::retired;

ModelWorker::ModelWorker() = default;
ModelWorker::~ModelWorker() = default;

bool ModelWorker::Start(IDXGIAdapter* adapter, ID3D11Device* renderDevice,
                        const std::filesystem::path& directory, bool regularizeB)
{
    if (!renderDevice || (renderDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)) return false;
    {
        std::lock_guard guard(Impl::retiredLock);
        std::erase_if(Impl::retired, [](const auto& job) { return job->finished.load(std::memory_order_acquire); });
    }
    try {
        auto impl = std::make_unique<Impl>();
        impl->regularizeB = regularizeB;
        impl->thread = std::thread([job = impl.get(), adapter = ComPtr<IDXGIAdapter>(adapter),
                                   device = ComPtr<ID3D11Device>(renderDevice), directory] {
            job->Run(adapter.Get(), device.Get(), directory);
        });
        m_impl = std::move(impl);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void ModelWorker::Stop(bool fast)
{
    if (m_impl && !m_impl->finished.load(std::memory_order_acquire) && fast && PinRenderer()) {
        m_impl->RequestStop();
        std::lock_guard guard(Impl::retiredLock);
        std::erase_if(Impl::retired, [](const auto& job) { return job->finished.load(std::memory_order_acquire); });
        Impl::retired.push_back(std::move(m_impl));
        return;
    }
    m_impl.reset();
}

void ModelWorker::Invalidate()
{
    if (!m_impl) return;
    std::lock_guard guard(m_impl->lock);
    ++m_impl->generation;
    m_impl->input.reset();
    m_impl->result.Reset();
    m_impl->resultParameters.reset();
}

bool ModelWorker::ReadyForInput() const
{
    if (!m_impl || m_impl->state.load(std::memory_order_acquire) != State::Ready) return false;
    std::lock_guard guard(m_impl->lock);
    return !m_impl->input && !m_impl->running && !m_impl->stopping;
}

bool ModelWorker::Submit(std::unique_ptr<Image> image)
{
    if (!m_impl || m_impl->state.load(std::memory_order_acquire) != State::Ready) return false;
    {
        std::lock_guard guard(m_impl->lock);
        if (m_impl->input || m_impl->running || m_impl->stopping) return false;
        m_impl->input = std::move(image);
    }
    m_impl->wake.notify_one();
    return true;
}

ComPtr<ID3D11ShaderResourceView> ModelWorker::TakeResult()
{
    if (!m_impl) return {};
    std::unique_lock guard(m_impl->lock, std::try_to_lock);
    if (!guard.owns_lock()) return {};
    m_impl->resultParameters.reset();
    return std::move(m_impl->result);
}

FrameResult ModelWorker::TryTakeFrameResult()
{
    if (!m_impl) return {};
    std::unique_lock guard(m_impl->lock, std::try_to_lock);
    if (!guard.owns_lock() || !m_impl->result || !m_impl->resultParameters) return {};
    return {std::move(m_impl->result), std::move(m_impl->resultParameters)};
}

ComPtr<ID3D11ShaderResourceView> ModelWorker::EvaluateFrame(std::unique_ptr<Image> image)
{
    return EvaluateFrameWithParameters(std::move(image)).view;
}

FrameResult ModelWorker::EvaluateFrameWithParameters(std::unique_ptr<Image> image)
{
    if (!image || GetState() != State::Ready) return {};
    auto& worker = *m_impl;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    std::unique_lock guard(worker.lock);
    const auto ready = [&] {
        return !worker.stopping && worker.state.load(std::memory_order_acquire) == State::Ready;
    };
    const auto idle = [&] { return !ready() || (!worker.input && !worker.running); };
    if (!worker.wake.wait_until(guard, deadline, idle) || !ready()) return {};

    worker.result.Reset();
    worker.resultParameters.reset();
    worker.input = std::move(image);
    worker.wake.notify_all();
    if (!worker.wake.wait_until(guard, deadline, idle)) {
        // Drop an overdue test result rather than applying it to another frame.
        ++worker.generation;
        worker.input.reset();
        return {};
    }
    if (!ready()) return {};
    return {std::move(worker.result), std::move(worker.resultParameters)};
}

ID3D11PixelShader* ModelWorker::Shader() const
{
    return GetState() == State::Ready ? m_impl->shader.Get() : nullptr;
}

ModelWorker::State ModelWorker::GetState() const
{
    return m_impl ? m_impl->state.load(std::memory_order_acquire) : State::Idle;
}

std::wstring ModelWorker::Failure() const
{
    // Failed is published after the worker has written its diagnostic.
    if (GetState() != State::Failed) return {};
    const std::string_view message(m_impl->failure.data());
    return {message.begin(), message.end()};
}

} // namespace svdlut
