// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <dml_provider_factory.h>
#include <flatbuffers/flatbuffers.h>
#include <json.hpp>

#include "../../Source/DLSS/DirectMLGraph/DmlGraphDesc_generated.h"
#include "../../Source/DLSS/ZipDepthPack.h"
#include "../../Source/DLSS/ZipDepthNative.h"
#include "ConverterResources.h"

#include <objbase.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <system_error>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using ZipDepthPack::Side;
constexpr std::size_t Pixels = Side * Side;

void Check(HRESULT hr, const char* stage)
{
    if (FAILED(hr)) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(hr));
        throw std::runtime_error(std::string(stage) + ": " + code);
    }
}

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
    fs::path path = fs::temp_directory_path() / (L"mpcvr-zipdepth-" + UniqueSuffix());

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
        {ZIPDEPTH_ORT, L"onnxruntime.dll"}, {ZIPDEPTH_ORT_SHARED, L"onnxruntime_providers_shared.dll"},
        {ZIPDEPTH_MSVCP, L"msvcp140.dll"}, {ZIPDEPTH_MSVCP_1, L"msvcp140_1.dll"},
        {ZIPDEPTH_VCRUNTIME, L"vcruntime140.dll"}, {ZIPDEPTH_VCRUNTIME_1, L"vcruntime140_1.dll"}
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

void ValidateTensor(const OrtApi* api, OrtSession* session, bool input)
{
    std::size_t count = 0;
    Check(api, input ? api->SessionGetInputCount(session, &count) : api->SessionGetOutputCount(session, &count));
    if (count != 1) throw std::runtime_error("ZipDepth requires one input and one output");
    OrtAllocator* allocator = nullptr;
    Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
    char* name = nullptr;
    Check(api, input ? api->SessionGetInputName(session, 0, allocator, &name)
                     : api->SessionGetOutputName(session, 0, allocator, &name));
    const auto release = [allocator](char* p) { allocator->Free(allocator, p); };
    const std::unique_ptr<char, decltype(release)> owner(name, release);
    if (!name || std::string_view(name) != (input ? "image" : "depth"))
        throw std::runtime_error("ZipDepth requires image and depth tensor names");

    OrtHandle<OrtTypeInfo, &OrtApi::ReleaseTypeInfo> type(api);
    Check(api, input ? api->SessionGetInputTypeInfo(session, 0, &type.value)
                     : api->SessionGetOutputTypeInfo(session, 0, &type.value));
    const OrtTensorTypeAndShapeInfo* tensor = nullptr;
    Check(api, api->CastTypeInfoToTensorInfo(type.value, &tensor));
    if (!tensor) throw std::runtime_error("ZipDepth inputs and outputs must be tensors");
    ONNXTensorElementDataType element{};
    Check(api, api->GetTensorElementType(tensor, &element));
    Check(api, api->GetDimensionsCount(tensor, &count));
    if (element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || count != 4)
        throw std::runtime_error("ZipDepth requires rank-four float32 tensors");
    std::array<int64_t, 4> actual{};
    Check(api, api->GetDimensions(tensor, actual.data(), actual.size()));
    const std::array<int64_t, 4> expected{1, input ? 3 : 1, Side, Side};
    if (actual != expected) throw std::runtime_error("ZipDepth requires fixed 384x384 tensors in NCHW order");
}

std::vector<float> TestImage()
{
    std::vector<float> image(3 * Pixels);
    for (std::size_t y = 0; y < Side; ++y) {
        for (std::size_t x = 0; x < Side; ++x) {
            const auto i = y * Side + x;
            image[i] = float(x) / (Side - 1);
            image[Pixels + i] = float(y) / (Side - 1);
            image[2 * Pixels + i] = 0.125f + 0.75f * ((x / 32 + y / 32) % 2);
        }
    }

    return image;
}

std::vector<float> ValidateExecution(const OrtApi* api, OrtSession* session)
{
    auto image = TestImage();
    OrtAllocator* allocator = nullptr;
    Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
    OrtHandle<OrtMemoryInfo, &OrtApi::ReleaseMemoryInfo> memory(api);
    Check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory.value));
    OrtHandle<OrtValue, &OrtApi::ReleaseValue> input(api), output(api);
    constexpr int64_t inputShape[]{1, 3, Side, Side}, outputShape[]{1, 1, Side, Side};
    constexpr auto fp32 = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    Check(api, api->CreateTensorWithDataAsOrtValue(memory.value, image.data(), image.size() * sizeof(float),
        inputShape, std::size(inputShape), fp32, &input.value));
    Check(api, api->CreateTensorAsOrtValue(allocator, outputShape, std::size(outputShape), fp32, &output.value));
    const char* inputName = "image";
    const char* outputName = "depth";
    const OrtValue* inputValue = input.value;
    Check(api, api->Run(session, nullptr, &inputName, &inputValue, 1, &outputName, 1, &output.value));
    void* data = nullptr;
    Check(api, api->GetTensorMutableData(output.value, &data));
    const auto* depth = static_cast<const float*>(data);
    if (!std::all_of(depth, depth + Pixels, [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("ZipDepth returned a non-finite depth tensor");

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
    if (count != 1) throw std::runtime_error("ONNX must run as one fused DirectML graph");
    return {depth, depth + Pixels};
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
    Check(api, api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "MPC-VR ZipDepth converter", &env.value));
    Check(api, api->CreateSessionOptions(&options.value));
    Check(api, api->SetSessionExecutionMode(options.value, ORT_SEQUENTIAL));
    Check(api, api->DisableMemPattern(options.value));
    Check(api, api->SetIntraOpNumThreads(options.value, 1));
    Check(api, api->SetSessionGraphOptimizationLevel(options.value, ORT_ENABLE_EXTENDED));
    Check(api, api->AddSessionConfigEntry(options.value, "session.disable_cpu_ep_fallback", "1"));
    Check(api, api->AddSessionConfigEntry(options.value, "ep.dml.enable_graph_serialization", "true"));
    Check(api, api->EnableProfiling(options.value, L"zipdepth"));
    Check(api, dml->SessionOptionsAppendExecutionProvider_DML1(options.value, ml.Get(), queue.Get()));
    OrtHandle<OrtSession, &OrtApi::ReleaseSession> session(api);
    Check(api, api->CreateSession(env.value, model.c_str(), options.value, &session.value));
    ValidateTensor(api, session.value, true);
    ValidateTensor(api, session.value, false);
    return ValidateExecution(api, session.value);
}

void ValidateNative(const fs::path& model, const fs::path& directml, std::span<const float> reference)
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
    constexpr auto inBytes = 3 * Pixels * sizeof(float), outBytes = Pixels * sizeof(float);
    const auto upload = makeBuffer(inBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const auto input = makeBuffer(inBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto output = makeBuffer(outBytes, D3D12_HEAP_TYPE_DEFAULT, uav);
    const auto readback = makeBuffer(outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const ZipDepthNative native(device.Get(), queue.Get(), input.Get(), output.Get(), model, directml);

    const auto image = TestImage();
    const D3D12_RANGE empty{0, 0}, uploaded{0, inBytes}, downloaded{0, outBytes};
    void* mapped = nullptr;
    Check(upload->Map(0, &empty, &mapped), "Map native test input");
    std::memcpy(mapped, image.data(), inBytes);
    upload->Unmap(0, &uploaded);

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
    list->CopyBufferRegion(input.Get(), 0, upload.Get(), 0, inBytes);
    transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, uav);
    native.Record(list.Get());
    transition(output.Get(), uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outBytes);
    Check(list->Close(), "Close native command list");
    Check(fence->SetEventOnCompletion(1, event.get()), "Set native completion event");
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), 1), "Signal native fence");
    CheckWin(WaitForSingleObject(event.get(), INFINITE) == WAIT_OBJECT_0, "Wait for native DirectML");
    Check(device->GetDeviceRemovedReason(), "Native DirectML device status");
    Check(readback->Map(0, &downloaded, &mapped), "Map native test depth");
    const auto* depth = static_cast<const float*>(mapped);
    float maxError = 0;
    bool finite = true;
    for (std::size_t i = 0; i < Pixels; ++i) {
        finite &= std::isfinite(depth[i]);
        maxError = std::max(maxError, std::abs(depth[i] - reference[i]));
    }
    readback->Unmap(0, &empty);
    if (!finite || maxError > 0.001f)
        throw std::runtime_error("Native DirectML pack does not match the ONNX depth output");
    std::cout << "Native DirectML depth max absolute difference: " << maxError << std::endl;
}

void WritePack(const fs::path& graphDirectory, const fs::path& destination,
    const fs::path& directml, std::span<const float> reference)
{
    unsigned partitions = 0;
    for (const auto& entry : fs::directory_iterator(graphDirectory)) {
        if (entry.is_regular_file() && entry.path().filename().wstring().starts_with(L"Partition_") &&
            entry.path().extension() == L".bin") ++partitions;
    }
    if (partitions != 1) throw std::runtime_error("Expected exactly one exported DirectML partition");

    std::map<std::string, ZipDepthPack::Bytes, std::less<>> files;
    const auto graphPath = graphDirectory / ZipDepthPack::GraphFile;
    const auto& graph = files.emplace(ZipDepthPack::GraphFile, ZipDepthPack::ReadFile(graphPath)).first->second;
    flatbuffers::Verifier verifier(graph.data(), graph.size());
    if (!dml::ir::VerifyDmlGraphDescBuffer(verifier)) throw std::runtime_error("Invalid exported DirectML graph");
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
        if (!ZipDepthPack::ValidName(filename) || filename == ZipDepthPack::GraphFile)
            throw std::runtime_error("Invalid DirectML weight name");
        if (files.contains(filename)) continue;
        const auto utf8 = std::u8string(reinterpret_cast<const char8_t*>(filename.data()), filename.size());
        auto bytes = ZipDepthPack::ReadFile(graphDirectory / fs::path(utf8), ZipDepthPack::Limit - totalBytes);
        if (bytes.size() % 4) throw std::runtime_error("Unaligned DirectML weight");
        totalBytes += bytes.size();
        files.emplace(filename, std::move(bytes));
    }
    ZipDepthPack::Entries entries;
    for (const auto& [name, bytes] : files) entries.emplace(name, bytes);

    const fs::path staging = destination.wstring() + L".partial-" + UniqueSuffix();
    try {
        std::ofstream file(staging, std::ios::binary);
        ZipDepthPack::Write(file, entries);
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

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--licenses") {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(ZIPDEPTH_LICENSES), MAKEINTRESOURCEW(10));
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
        std::cout << "Usage: zipdepth-convert.exe INPUT.onnx OUTPUT.dmlpack\n"
                     "       zipdepth-convert.exe --licenses\n";
        return 0;
    }
    if (argc != 3) {
        std::cerr << "Usage: zipdepth-convert.exe INPUT.onnx OUTPUT.dmlpack\n";
        return 2;
    }
    try {
        const auto input = fs::absolute(argv[1]);
        const auto output = fs::absolute(argv[2]);
        if (_wcsicmp(input.extension().c_str(), L".onnx") != 0 ||
            _wcsicmp(output.extension().c_str(), L".dmlpack") != 0)
            throw std::runtime_error("Expected .onnx input and .dmlpack output");
        if (!fs::is_regular_file(input)) throw std::runtime_error("Input ONNX file is missing");
        if (!fs::is_directory(output.parent_path())) throw std::runtime_error("Output directory is missing");

        const auto directml = ExecutableDirectory() / L"DirectML.dll";
        TemporaryDirectory temp;
        const auto model = temp.path / L"zipdepth.onnx";
        ExtractRuntime(temp.path);
        // Use an isolated model directory because ORT exports the graph beside the current working directory.
        fs::copy_file(input, model);
        fs::current_path(temp.path);
        const auto reference = ExportGraph(model, temp.path, directml);
        WritePack(temp.path / L"zipdepth", output, directml, reference);
        std::cout << "ZipDepth model pack ready." << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Conversion failed: " << error.what() << '\n';
        return 1;
    }
}
