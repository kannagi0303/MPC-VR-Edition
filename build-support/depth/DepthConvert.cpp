// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef DEPTH_ONNX_MAINTENANCE
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <dml_provider_factory.h>
#include <flatbuffers/flatbuffers.h>
#include <json.hpp>

#include "../../Source/Depth/DirectMLGraph/DmlGraphDesc_generated.h"
#include "../../Source/Depth/DepthNative.h"
#include "../../Source/Depth/DepthStreamContract.h"
#include "DmlGraphDiagnostics.h"
#endif
#include "../../Source/Depth/DepthPack.h"
#include "../../Source/Depth/DepthSplitPack.h"
#include "ConverterResources.h"
#include "SafetensorsDepth.h"

#include <objbase.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <system_error>

namespace fs = std::filesystem;
#ifdef DEPTH_ONNX_MAINTENANCE
using Microsoft::WRL::ComPtr;
void AdaptDepthOnnx(const fs::path& input, const fs::path& output);
#endif

void Check(HRESULT hr, const char* stage)
{
    if (FAILED(hr)) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(hr));
        throw std::runtime_error(std::string(stage) + ": " + code);
    }
}

#ifdef DEPTH_ONNX_MAINTENANCE
void CheckWin(bool success, const char* stage)
{
    if (!success) throw std::system_error(GetLastError(), std::system_category(), stage);
}

void Check(const OrtApi* api, OrtStatus* status)
{
    if (!status) return;
    const std::unique_ptr<OrtStatus, decltype(api->ReleaseStatus)> owner(status, api->ReleaseStatus);
    throw std::runtime_error(api->GetErrorMessage(status));
}

template<class T, auto Release>
struct OrtHandle {
    const OrtApi* api;
    T* value = nullptr;
    explicit OrtHandle(const OrtApi* api) : api(api) {}
    ~OrtHandle() { if (value) (api->*Release)(value); }
    OrtHandle(const OrtHandle&) = delete;
};

struct Library {
    HMODULE handle;
    explicit Library(const fs::path& path)
        : handle(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        if (!handle) throw std::system_error(GetLastError(), std::system_category(), path.filename().string());
    }
    ~Library() { FreeLibrary(handle); }
    Library(const Library&) = delete;

    template<class Function>
    Function Symbol(const char* name) const
    {
        const auto address = GetProcAddress(handle, name);
        if (!address) throw std::runtime_error(std::string("Missing DLL export ") + name);
        return reinterpret_cast<Function>(address);
    }
};

std::wstring UniqueSuffix()
{
    GUID id{};
    Check(CoCreateGuid(&id), "CoCreateGuid");
    wchar_t text[40]{};
    if (!StringFromGUID2(id, text, std::size(text))) throw std::runtime_error("Cannot format temporary name");
    return text;
}

struct TemporaryDirectory {
    fs::path original = fs::current_path();
    fs::path path = fs::temp_directory_path() / (L"mpcvr-depth-" + UniqueSuffix());

    TemporaryDirectory()
    {
        if (!fs::create_directory(path)) throw std::runtime_error("Cannot create private conversion directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::current_path(original, error);
        fs::remove_all(path, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
};

fs::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    CheckWin(size && size < path.size(), "GetModuleFileNameW");
    path.resize(size);
    return fs::path(path).parent_path();
}

void ExtractRuntime(const fs::path& directory)
{
    const struct { int id; const wchar_t* name; } dlls[]{
        {DEPTH_CONVERTER_ORT, L"onnxruntime.dll"}, {DEPTH_CONVERTER_ORT_SHARED, L"onnxruntime_providers_shared.dll"},
        {DEPTH_CONVERTER_MSVCP, L"msvcp140.dll"}, {DEPTH_CONVERTER_MSVCP_1, L"msvcp140_1.dll"},
        {DEPTH_CONVERTER_VCRUNTIME, L"vcruntime140.dll"}, {DEPTH_CONVERTER_VCRUNTIME_1, L"vcruntime140_1.dll"}
    };
    const auto module = GetModuleHandleW(nullptr);
    for (const auto& dll : dlls) {
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(dll.id), MAKEINTRESOURCEW(10));
        CheckWin(resource != nullptr, "FindResourceW for converter runtime");
        const auto data = LockResource(LoadResource(module, resource));
        const auto size = SizeofResource(module, resource);
        if (!data || !size) throw std::runtime_error("Missing embedded converter runtime");
        std::ofstream file(directory / dll.name, std::ios::binary);
        if (!file.write(static_cast<const char*>(data), size))
            throw std::runtime_error("Cannot extract converter runtime");
        file.close();
        if (!file) throw std::runtime_error("Cannot finish extracting converter runtime");
    }
}

ComPtr<ID3D12Device> OpenDevice()
{
    ComPtr<IDXGIFactory1> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> first;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto hr = factory->EnumAdapters1(i, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        Check(hr, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 desc{};
        Check(adapter->GetDesc1(&desc), "GetDesc1");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()))))
            continue;
        if (desc.VendorId == 0x10de) return device;
        if (!first) first = std::move(adapter);
    }
    if (!first) throw std::runtime_error("No Direct3D 12 hardware adapter is available");
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(first.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())),
        "D3D12CreateDevice");
    return device;
}

std::vector<float> TestImage(unsigned step = 0)
{
    const auto width = DepthPack::Width, height = DepthPack::Height;
    const auto Pixels = DepthPack::Pixels;
    std::vector<float> image(3 * Pixels);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto i = y * width + x;
            image[i] = float((x + 19 * step) % width) / (width - 1);
            image[Pixels + i] = float((y + 11 * step) % height) / (height - 1);
            image[2 * Pixels + i] = 0.125f + 0.75f * (((x + step * 9) / 32 + y / 32) % 2);
        }
    }

    return image;
}

void ValidateProfile(const OrtApi* api, OrtSession* session, unsigned runs)
{
    OrtAllocator* allocator = nullptr;
    Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
    char* profile = nullptr;
    Check(api, api->SessionEndProfiling(session, allocator, &profile));
    const auto release = [allocator](char* p) { allocator->Free(allocator, p); };
    const std::unique_ptr<char, decltype(release)> owner(profile, release);
    if (!profile || !*profile) throw std::runtime_error("ONNX Runtime did not write a profile");
    std::ifstream trace{fs::path(profile)};
    const auto events = nlohmann::json::parse(trace);
    if (!events.is_array()) throw std::runtime_error("Invalid ONNX Runtime profile");
    unsigned count = 0;
    for (const auto& event : events) {
        if (event.value("cat", "") != std::string_view("Node") ||
            !event.value("name", std::string{}).ends_with("_kernel_time")) continue;
        const auto& args = event.at("args");
        const auto op = args.value("op_name", std::string{});
        if (op == "MemcpyFromHost" || op == "MemcpyToHost" || op == "Memcpy") continue;
        if (!op.starts_with("DmlFusedNode_") || args.value("provider", std::string{}) != "DmlExecutionProvider")
            throw std::runtime_error("ONNX contains computation outside the DirectML fused graph");
        ++count;
    }
    if (count != runs) throw std::runtime_error("ONNX must run as one fused DirectML graph");
}

struct TensorSpec {
    std::string name;
    ONNXTensorElementDataType type{};
    std::vector<int64_t> dims;
    size_t bytes = 0;
};

std::vector<TensorSpec> ReadSpecs(const OrtApi* api, OrtSession* session, bool input)
{
    size_t count = 0;
    Check(api, input ? api->SessionGetInputCount(session, &count) : api->SessionGetOutputCount(session, &count));
    if (!count || count > depthstream::MaxStates + 1) throw std::runtime_error("Invalid depth tensor count");
    OrtAllocator* allocator = nullptr;
    Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
    std::vector<TensorSpec> specs;
    for (size_t i = 0; i < count; ++i) {
        char* name = nullptr;
        Check(api, input ? api->SessionGetInputName(session, i, allocator, &name) : api->SessionGetOutputName(session, i, allocator, &name));
        const auto release = [allocator](char* p) { allocator->Free(allocator, p); };
        const std::unique_ptr<char, decltype(release)> owner(name, release);
        if (!name || !*name) throw std::runtime_error("Missing depth tensor name");
        OrtHandle<OrtTypeInfo, &OrtApi::ReleaseTypeInfo> info(api);
        Check(api, input ? api->SessionGetInputTypeInfo(session, i, &info.value) : api->SessionGetOutputTypeInfo(session, i, &info.value));
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        Check(api, api->CastTypeInfoToTensorInfo(info.value, &tensor));
        if (!tensor) throw std::runtime_error("Expected depth tensor");
        TensorSpec spec;
        spec.name = name;
        Check(api, api->GetTensorElementType(tensor, &spec.type));
        size_t rank = 0;
        Check(api, api->GetDimensionsCount(tensor, &rank));
        if (!rank || rank > 8) throw std::runtime_error("Unsupported depth tensor rank");
        spec.dims.resize(rank);
        Check(api, api->GetDimensions(tensor, spec.dims.data(), rank));
        spec.bytes = spec.type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? 4 : spec.type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ? 2 : 0;
        if (!spec.bytes) throw std::runtime_error("Expected FP32 or FP16 depth tensor");
        for (const auto d : spec.dims) {
            if (d <= 0 || uint64_t(d) > depthstream::MaxStateBytes / spec.bytes)
                throw std::runtime_error("Dynamic or oversized depth tensor");
            spec.bytes *= static_cast<size_t>(d);
        }
        if (std::any_of(specs.begin(), specs.end(), [&](const auto& x) { return x.name == spec.name; }))
            throw std::runtime_error("Duplicate depth tensor name");
        specs.push_back(std::move(spec));
    }
    return specs;
}

// Shared test sequence: two full W4 windows, then reset and replay the first two frames.
constexpr unsigned StreamFrames = 10;
unsigned ImageStep(unsigned i) { return i < 8 ? i : i - 8; }
bool ResetStep(unsigned i) { return i == 0 || i == 8; }

std::vector<float> ValidateStream(const OrtApi* api, OrtSession* session)
{
    const auto inputs = ReadSpecs(api, session, true), outputs = ReadSpecs(api, session, false);
    if (inputs.size() != outputs.size() || inputs.size() < 2)
        throw std::runtime_error("Invalid stream input/output count");
    auto find = [](const auto& specs, const std::string& name) -> size_t {
        const auto it = std::find_if(specs.begin(), specs.end(), [&](const auto& x) { return x.name == name; });
        if (it == specs.end()) throw std::runtime_error("Missing stream tensor: " + name);
        return static_cast<size_t>(it - specs.begin());
    };
    const auto imageIndex = find(inputs, "image"), depthIndex = find(outputs, "depth");
    constexpr auto fp32 = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    const auto h = DepthPack::Height, w = DepthPack::Width;
    if (inputs[imageIndex].type != fp32 || outputs[depthIndex].type != fp32 ||
        inputs[imageIndex].dims != std::vector<int64_t>{1, 3, h, w} ||
        outputs[depthIndex].dims != std::vector<int64_t>{1, 1, h, w})
        throw std::runtime_error("Expected fixed 352x192 stream image and depth tensors");
    const auto Pixels = DepthPack::Pixels;
    std::vector<size_t> next(inputs.size());
    size_t stateBytes = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (i == imageIndex) continue;
        const auto& a = inputs[i];
        next[i] = find(outputs, depthstream::Present(a.name));
        const auto& b = outputs[next[i]];
        if (a.type != b.type || a.dims != b.dims) throw std::runtime_error("Mismatched stream state: " + a.name);
        stateBytes += a.bytes;
        if (stateBytes > depthstream::MaxStateBytes) throw std::runtime_error("Stream state exceeds limit");
    }
    OrtAllocator* allocator = nullptr;
    Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
    using Value = OrtHandle<OrtValue, &OrtApi::ReleaseValue>;
    auto allocate = [&](const TensorSpec& spec) {
        auto v = std::make_unique<Value>(api);
        Check(api, api->CreateTensorAsOrtValue(allocator, spec.dims.data(), spec.dims.size(), spec.type, &v->value));
        return v;
    };
    std::vector<std::unique_ptr<Value>> in;
    std::vector<const char*> inNames, outNames;
    for (const auto& spec : inputs) { in.push_back(allocate(spec)); inNames.push_back(spec.name.c_str()); }
    for (const auto& spec : outputs) outNames.push_back(spec.name.c_str());
    std::vector<float> references;
    for (unsigned frame = 0; frame < StreamFrames; ++frame) {
        auto image = TestImage(ImageStep(frame));
        for (size_t i = 0; i < inputs.size(); ++i) {
            void* data = nullptr;
            Check(api, api->GetTensorMutableData(in[i]->value, &data));
            if (i == imageIndex) std::memcpy(data, image.data(), inputs[i].bytes);
            else if (ResetStep(frame)) std::memset(data, 0, inputs[i].bytes);
        }
        std::vector<const OrtValue*> inputValues;
        for (const auto& v : in) inputValues.push_back(v->value);
        std::vector<std::unique_ptr<Value>> out;
        std::vector<OrtValue*> outputValues;
        for (const auto& spec : outputs) { out.push_back(allocate(spec)); outputValues.push_back(out.back()->value); }
        Check(api, api->Run(session, nullptr, inNames.data(), inputValues.data(), inputs.size(), outNames.data(), outputs.size(), outputValues.data()));
        void* data = nullptr;
        Check(api, api->GetTensorMutableData(out[depthIndex]->value, &data));
        const auto* depth = static_cast<const float*>(data);
        if (!std::all_of(depth, depth + Pixels, [](float x) { return std::isfinite(x); }))
            throw std::runtime_error("Stream returned non-finite depth");
        references.insert(references.end(), depth, depth + Pixels);
        for (size_t i = 0; i < inputs.size(); ++i) if (i != imageIndex) in[i] = std::move(out[next[i]]);
    }
    ValidateProfile(api, session, StreamFrames);
    // The explicit reset must restore cold-start behavior, not just agree with
    // a second runner that could be leaking the same state.
    for (size_t i = 0; i < 2 * Pixels; ++i)
        if (std::abs(references[i] - references[8 * Pixels + i]) > 0.001f)
            throw std::runtime_error("ONNX stream reset did not reproduce cold-start depth");
    std::cout << "Stream state bytes per bank: " << stateBytes << std::endl;
    return references;
}

std::vector<float> ExportGraph(const fs::path& model, const fs::path& runtime, const fs::path& dmlPath)
{
    const Library directml(dmlPath);
    const Library ort(runtime / L"onnxruntime.dll");
    const auto* base = ort.Symbol<decltype(&OrtGetApiBase)>("OrtGetApiBase")();
    if (std::string_view(base->GetVersionString()) != "1.24.4")
        throw std::runtime_error("Unexpected embedded ONNX Runtime version");
    const auto* api = base->GetApi(ORT_API_VERSION);
    if (!api) throw std::runtime_error("Incompatible ONNX Runtime API");
    const void* provider = nullptr;
    Check(api, api->GetExecutionProviderApi("DML", ORT_API_VERSION, &provider));
    if (!provider) throw std::runtime_error("Missing ONNX Runtime DirectML provider");
    const auto* dml = static_cast<const OrtDmlApi*>(provider);

    const auto device = OpenDevice();
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf())), "CreateCommandQueue");
    ComPtr<IDMLDevice> ml;
    constexpr GUID iid{0x6dbd6437, 0x96fd, 0x423f, {0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f}};
    const auto create = directml.Symbol<decltype(&DMLCreateDevice)>("DMLCreateDevice");
    Check(create(device.Get(), DML_CREATE_DEVICE_FLAG_NONE, iid, reinterpret_cast<void**>(ml.GetAddressOf())),
        "DMLCreateDevice");

    OrtHandle<OrtEnv, &OrtApi::ReleaseEnv> env(api);
    OrtHandle<OrtSessionOptions, &OrtApi::ReleaseSessionOptions> options(api);
    Check(api, api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "MPC-VR Depth Pro 2 converter", &env.value));
    Check(api, api->CreateSessionOptions(&options.value));
    Check(api, api->SetSessionExecutionMode(options.value, ORT_SEQUENTIAL));
    Check(api, api->DisableMemPattern(options.value));
    Check(api, api->SetIntraOpNumThreads(options.value, 1));
    Check(api, api->SetSessionGraphOptimizationLevel(options.value, ORT_ENABLE_EXTENDED));
    Check(api, api->AddSessionConfigEntry(options.value, "session.disable_cpu_ep_fallback", "1"));
    Check(api, api->AddSessionConfigEntry(options.value, "ep.dml.enable_graph_serialization", "true"));
    Check(api, api->EnableProfiling(options.value, L"depth"));
    Check(api, dml->SessionOptionsAppendExecutionProvider_DML1(options.value, ml.Get(), queue.Get()));
    OrtHandle<OrtSession, &OrtApi::ReleaseSession> session(api);
    Check(api, api->CreateSession(env.value, model.c_str(), options.value, &session.value));
    return ValidateStream(api, session.value);
}

void ValidateNative(const fs::path& model, const fs::path& directml, std::span<const float> reference,
    const fs::path& splitWeights = {})
{
    const auto device = OpenDevice();
    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.GetAddressOf())), "Create native queue");
    auto makeBuffer = [&device](std::size_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
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
            IID_PPV_ARGS(result.GetAddressOf())), "Create native test buffer");
        return result;
    };
    const auto pack = DepthPack::ReadFile(model, DepthPack::Limit + depthsplit::PrefixSize);
    const auto weights = splitWeights.empty() ? DepthPack::Bytes{} :
        DepthPack::ReadFile(splitWeights, DepthPack::Limit + depthsplit::PrefixSize);
    const auto Pixels = DepthPack::Pixels;
    const auto inBytes = 3 * Pixels * sizeof(float), outBytes = Pixels * sizeof(float);
    const auto upload = makeBuffer(inBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const auto input = makeBuffer(inBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto output = makeBuffer(outBytes, D3D12_HEAP_TYPE_DEFAULT, uav);
    const auto readback = makeBuffer(outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const DepthNative native(device.Get(), queue.Get(), input.Get(), output.Get(), pack, directml, weights);

    constexpr unsigned frames = StreamFrames;
    if (reference.size() != size_t(frames) * Pixels) throw std::runtime_error("Native stream reference length mismatch");
    const D3D12_RANGE empty{0, 0}, uploaded{0, inBytes}, downloaded{0, outBytes};
    void* mapped = nullptr;

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(allocator.GetAddressOf())), "Create native allocator");
    Check(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf())),
        "Create native command list");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())), "Create native fence");
    const std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
    CheckWin(bool(event), "Create native event");
    auto transition = [&list](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &barrier);
    };
    float maxError = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (frame) {
            Check(allocator->Reset(), "Reset native allocator");
            Check(list->Reset(allocator.Get(), nullptr), "Reset native list");
            transition(input.Get(), uav, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, uav);
        }
        const auto image = TestImage(ImageStep(frame));
        Check(upload->Map(0, &empty, &mapped), "Map native test input");
        std::memcpy(mapped, image.data(), inBytes);
        upload->Unmap(0, &uploaded);
        list->CopyBufferRegion(input.Get(), 0, upload.Get(), 0, inBytes);
        transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, uav);
        native.Record(list.Get(), ResetStep(frame));
        transition(output.Get(), uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outBytes);
        Check(list->Close(), "Close native command list");
        const UINT64 fenceValue = frame + 1;
        Check(fence->SetEventOnCompletion(fenceValue, event.get()), "Set native completion event");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), fenceValue), "Signal native fence");
        CheckWin(WaitForSingleObject(event.get(), INFINITE) == WAIT_OBJECT_0, "Wait for native DirectML");
        Check(device->GetDeviceRemovedReason(), "Native DirectML device status");
        Check(readback->Map(0, &downloaded, &mapped), "Map native test depth");
        const auto* depth = static_cast<const float*>(mapped);
        bool finite = true;
        float error = 0;
        for (std::size_t i = 0; i < Pixels; ++i) {
            finite &= std::isfinite(depth[i]);
            error = std::max(error, std::abs(depth[i] - reference[size_t(frame) * Pixels + i]));
        }
        readback->Unmap(0, &empty);
        if (!finite || error > 0.001f)
            throw std::runtime_error("Native DirectML pack differs from ONNX at frame " + std::to_string(frame));
        maxError = std::max(maxError, error);
        std::cout << "Native DirectML frame " << frame << ": max absolute difference " << error << std::endl;
    }
    std::cout << "Native DirectML depth max absolute difference: " << maxError << std::endl;

}

void WritePack(const fs::path& graphDirectory, const fs::path& destination,
    const fs::path& directml, std::span<const float> reference, const fs::path& input)
{
    unsigned partitions = 0;
    for (const auto& entry : fs::directory_iterator(graphDirectory)) {
        if (entry.is_regular_file() && entry.path().filename().wstring().starts_with(L"Partition_") &&
            entry.path().extension() == L".bin") ++partitions;
    }
    if (partitions != 1) throw std::runtime_error("Expected exactly one exported DirectML partition");

    std::map<std::string, DepthPack::Bytes, std::less<>> files;
    const auto graphPath = graphDirectory / DepthPack::GraphFile;
    const auto& graph = files.emplace(DepthPack::GraphFile, DepthPack::ReadFile(graphPath)).first->second;
    flatbuffers::Verifier verifier(graph.data(), graph.size());
    if (!dml::ir::VerifyDmlGraphDescBuffer(verifier)) {
        // Save only the serialized graph (not the embedded ORT DLL, ONNX or
        // separate weight files) outside the temporary directory before cleanup.
        const fs::path failedGraph = input.wstring() + L".dmlgraph-failed.bin";
        std::error_code copyError;
        fs::copy_file(graphPath, failedGraph, fs::copy_options::overwrite_existing, copyError);
        if (copyError) {
            std::cerr << "Could not preserve rejected graph: " << copyError.message() << '\n';
        } else {
            const auto utf8 = failedGraph.u8string();
            std::cerr << "Rejected graph preserved: " << reinterpret_cast<const char*>(utf8.c_str()) << '\n';
        }
        std::cerr << "Serialized graph size: " << graph.size() << " bytes\n";
        try {
            const auto diagnostic = dmlgraphdiag::DescribeFailure(graph);
            std::cerr << "Graph diagnostic: " << diagnostic << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Graph diagnostic unavailable: " << error.what() << '\n';
        }
        throw std::runtime_error("Invalid exported DirectML graph");
    }
    const auto* root = dml::ir::GetDmlGraphDesc(graph.data());
    if (!root->nodes()) throw std::runtime_error("No nodes in exported DirectML graph");
    std::size_t totalBytes = graph.size();
    for (const auto* node : *root->nodes()) {
        const auto* constant = node->desc_as_ConstantNodeDesc();
        if (!constant) continue;
        const auto* named = constant->data_as_ConstantName();
        if (!named) continue;
        if (!named->name()) throw std::runtime_error("Unnamed DirectML weight");
        const auto filename = named->name()->str() + ".bin";
        if (!DepthPack::ValidName(filename) || filename == DepthPack::GraphFile)
            throw std::runtime_error("Invalid DirectML weight name");
        if (files.contains(filename)) continue;
        const auto utf8 = std::u8string(reinterpret_cast<const char8_t*>(filename.data()), filename.size());
        auto bytes = DepthPack::ReadFile(graphDirectory / fs::path(utf8), DepthPack::Limit - totalBytes);
        if (bytes.size() % 4) throw std::runtime_error("Unaligned DirectML weight");
        totalBytes += bytes.size();
        files.emplace(filename, std::move(bytes));
    }
    DepthPack::Entries entries;
    for (const auto& [name, bytes] : files) entries.emplace(name, bytes);

    const fs::path staging = destination.wstring() + L".partial-" + UniqueSuffix();
    try {
        std::ofstream file(staging, std::ios::binary);
        DepthPack::Write(file, entries);
        file.close();
        if (!file) throw std::runtime_error("Cannot finish writing model pack");
        ValidateNative(staging, directml, reference);
        CheckWin(MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
            "Publish model pack");
    } catch (...) {
        std::error_code error;
        fs::remove(staging, error);
        throw;
    }
}

#endif

void VerifyPair(const fs::path& source, const fs::path& graph, const fs::path& weights)
{
    const auto packedBytes = DepthPack::ReadFile(source);
    const auto graphBytes = DepthPack::ReadFile(graph, DepthPack::Limit + depthsplit::PrefixSize);
    const auto weightBytes = DepthPack::ReadFile(weights, DepthPack::Limit + depthsplit::PrefixSize);
    if (!depthsplit::SplitGraph(graphBytes)) throw std::runtime_error("Expected a split graph");
    const auto entries = DepthPack::Read(packedBytes);
    const auto paired = depthsplit::Open(graphBytes, weightBytes);
    if (paired.size() != entries.size()) throw std::runtime_error("Split entry count changed");
    for (const auto& [name, data] : entries)
        if (!std::ranges::equal(data, paired.at(name))) throw std::runtime_error("Split tensor bytes changed");
    std::cout << "[OK] Split model byte-for-byte verified.\n";
}

// Split only validated native pack bytes. This command does not load ORT or
// DirectML, create a GPU device, or export/compile the graph again.
void SplitPack(wchar_t** argv)
{
    const auto source = fs::absolute(argv[2]);
    const auto graph = fs::absolute(argv[3]);
    const auto weights = fs::absolute(argv[4]);
    if (_wcsicmp(source.extension().c_str(), L".dmlpack") ||
        _wcsicmp(graph.extension().c_str(), L".dmlgraph") ||
        _wcsicmp(weights.extension().c_str(), L".dmlweights") ||
        graph.parent_path() != weights.parent_path() || fs::exists(graph) || fs::exists(weights))
        throw std::runtime_error("Expected native pack and new graph/weights files in one directory");
    const auto bytes = DepthPack::ReadFile(source);
    const auto entries = DepthPack::Read(bytes);
    const auto parts = depthsplit::Partition(entries);
    GUID guid{};
    Check(CoCreateGuid(&guid), "Create split depth resource identifier");
    depthsplit::PairId id{};
    static_assert(sizeof(GUID) == std::tuple_size_v<depthsplit::PairId>);
    std::memcpy(id.data(), &guid, id.size());
    try {
        {
            std::ofstream out(graph, std::ios::binary);
            depthsplit::WritePart(out, parts.graph, id, false);
            out.close();
            if (!out) throw std::runtime_error("Cannot finish depth graph file");
        }
        {
            std::ofstream out(weights, std::ios::binary);
            depthsplit::WritePart(out, parts.weights, id, true);
            out.close();
            if (!out) throw std::runtime_error("Cannot finish depth weight file");
        }
        VerifyPair(source, graph, weights);
    } catch (...) {
        std::error_code ec;
        fs::remove(graph,ec);
        fs::remove(weights,ec);
        throw;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 6 && (std::wstring_view(argv[1]) == L"--from-safetensors" ||
                      std::wstring_view(argv[1]) == L"--make-recipe")) {
        try {
            if (std::wstring_view(argv[1]) == L"--from-safetensors")
                depthrecipe::Convert(argv[2], argv[3], argv[4], argv[5]);
            else depthrecipe::Generate(argv[2], argv[3], argv[4], argv[5]);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Safetensors conversion failed: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 5 && (std::wstring_view(argv[1]) == L"--split" ||
                      std::wstring_view(argv[1]) == L"--verify-pair")) {
        try {
            if (std::wstring_view(argv[1]) == L"--split") SplitPack(argv);
            else VerifyPair(argv[2], argv[3], argv[4]);
            return 0;
        }
        catch (const std::exception& error) { std::cerr << "Split failed: " << error.what() << '\n'; return 1; }
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--licenses") {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(DEPTH_CONVERTER_LICENSES), MAKEINTRESOURCEW(10));
        if (!resource) {
            std::cerr << "Converter license resource is missing.\n";
            return 1;
        }
        const auto data = static_cast<const char*>(LockResource(LoadResource(module, resource)));
        const auto size = SizeofResource(module, resource);
        if (!data || !size) {
            std::cerr << "Cannot read converter license resource.\n";
            return 1;
        }
        std::cout.write(data, size);
        return std::cout ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::cout << "Usage: depth-convert.exe --from-safetensors MODEL.safetensors CONFIG.json OUTPUT.dmlgraph OUTPUT.dmlweights\n"
#ifdef DEPTH_ONNX_MAINTENANCE
                     "       depth-convert.exe INPUT.onnx OUTPUT.dmlpack (maintenance build)\n"
                     "       depth-convert.exe --adapt-onnx SOURCE.onnx ADAPTED.onnx (maintenance build)\n"
                     "       depth-convert.exe --validate-split INPUT.onnx INPUT.dmlgraph INPUT.dmlweights (maintenance build)\n"
#endif
                     "       depth-convert.exe --make-recipe MODEL.safetensors CONFIG.json VERIFIED.dmlpack OUTPUT.recipe\n"
                     "       depth-convert.exe --split INPUT.dmlpack OUTPUT.dmlgraph OUTPUT.dmlweights\n"
                     "       depth-convert.exe --verify-pair INPUT.dmlpack INPUT.dmlgraph INPUT.dmlweights\n"
                     "       depth-convert.exe --licenses\n";
        return 0;
    }
#ifdef DEPTH_ONNX_MAINTENANCE
    if (argc == 4 && std::wstring_view(argv[1]) == L"--adapt-onnx") {
        fs::path staging;
        try {
            const auto input = fs::absolute(argv[2]);
            const auto output = fs::absolute(argv[3]);
            if (_wcsicmp(input.extension().c_str(), L".onnx") || _wcsicmp(output.extension().c_str(), L".onnx") ||
                fs::exists(output)) throw std::runtime_error("Expected source and new adapted .onnx files");
            staging = output.wstring() + L".partial-" + UniqueSuffix();
            AdaptDepthOnnx(input, staging);
            CheckWin(MoveFileExW(staging.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH) != 0, "Publish adapted ONNX");
            std::cout << "Adapted focal-free W4 ONNX for DirectML.\n";
            return 0;
        } catch (const std::exception& error) {
            std::error_code ec;
            if (!staging.empty()) fs::remove(staging, ec);
            std::cerr << "ONNX adaptation failed: " << error.what() << '\n';
            return 1;
        }
    }
    const bool validateSplit = argc == 5 && std::wstring_view(argv[1]) == L"--validate-split";
    if (argc != 3 && !validateSplit) {
        std::cerr << "Run depth-convert.exe --help for usage.\n";
        return 2;
    }
    try {
        const auto input = fs::absolute(argv[validateSplit ? 2 : 1]);
        const auto output = fs::absolute(argv[validateSplit ? 3 : 2]);
        const auto weights = validateSplit ? fs::absolute(argv[4]) : fs::path{};
        if (_wcsicmp(input.extension().c_str(), L".onnx") != 0 ||
            _wcsicmp(output.extension().c_str(), validateSplit ? L".dmlgraph" : L".dmlpack") != 0 ||
            (validateSplit && _wcsicmp(weights.extension().c_str(), L".dmlweights") != 0))
            throw std::runtime_error("Expected adapted ONNX and native model resource paths");
        if (!fs::is_regular_file(input)) throw std::runtime_error("Input ONNX file is missing");
        if (!fs::is_directory(output.parent_path())) throw std::runtime_error("Output directory is missing");

        const auto directml = ExecutableDirectory() / L"DirectML.dll";
        TemporaryDirectory temp;
        const auto model = temp.path / L"depth.onnx";
        ExtractRuntime(temp.path);
        // Use an isolated model directory because ORT exports the graph beside the current working directory.
        fs::copy_file(input, model);
        fs::current_path(temp.path);
        const auto reference = ExportGraph(model, temp.path, directml);
        if (validateSplit) {
            ValidateNative(output, directml, reference, weights);
            return 0;
        }
        WritePack(temp.path / L"depth", output, directml, reference, input);
        std::cout << "Depth Pro 2 model pack ready." << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Conversion failed: " << error.what() << '\n';
        return 1;
    }
#else
    std::cerr << "Run depth-convert.exe --help for usage. ONNX export requires the maintenance build.\n";
    return 2;
#endif
}
