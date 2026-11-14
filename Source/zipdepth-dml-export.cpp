#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "dml_provider_factory.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
#include "inputs.hpp"

struct Library {
    HMODULE handle;
    explicit Library(const fs::path& path)
        : handle(LoadLibraryExW(path.c_str(), nullptr,
                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
    {
        if (!handle) {
            const DWORD error = GetLastError();
            throw std::runtime_error("Failed to load " + path.filename().string() +
                                     " (Win32 " + std::to_string(error) + ")");
        }
    }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    ~Library() { FreeLibrary(handle); }

    template<class Function>
    Function symbol(const char* name) const
    {
        const auto fn = GetProcAddress(handle, name);
        if (!fn) throw std::runtime_error(std::string("Missing export ") + name);
        return reinterpret_cast<Function>(fn);
    }
};

void Check(const OrtApi* api, OrtStatus* status)
{
    if (!status) return;
    const std::unique_ptr<OrtStatus, decltype(api->ReleaseStatus)> owner(status, api->ReleaseStatus);
    throw std::runtime_error(api->GetErrorMessage(status));
}

void Check(HRESULT hr, const char* stage)
{
    if (SUCCEEDED(hr)) return;
    std::cerr << stage << " HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << '\n';
    throw std::runtime_error(stage);
}

template<class T, auto Release>
struct OrtObject {
    T* ptr = nullptr;
    const OrtApi* api;
    explicit OrtObject(const OrtApi* api) : api(api) {}
    OrtObject(const OrtObject&) = delete;
    OrtObject& operator=(const OrtObject&) = delete;
    ~OrtObject() { if (ptr) (api->*Release)(ptr); }
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

struct RunResult {
    std::vector<float> depth;
    fs::path profile;
};

RunResult RunModel(const OrtApi* api, const OrtDmlApi* dml, OrtEnv* env,
                   IDMLDevice* device, ID3D12CommandQueue* queue,
                   const fs::path& model, bool exportGraph)
{
    OrtObject<OrtSessionOptions, &OrtApi::ReleaseSessionOptions> options(api);
    Check(api, api->CreateSessionOptions(&options.ptr));
    Check(api, api->SetSessionExecutionMode(options.ptr, ORT_SEQUENTIAL));
    Check(api, api->DisableMemPattern(options.ptr));
    Check(api, api->SetIntraOpNumThreads(options.ptr, 1));
    Check(api, api->SetSessionGraphOptimizationLevel(options.ptr, ORT_ENABLE_EXTENDED));
    Check(api, api->AddSessionConfigEntry(options.ptr, "session.disable_cpu_ep_fallback", "1"));
    Check(api, dml->SessionOptionsAppendExecutionProvider_DML1(options.ptr, device, queue));
    const wchar_t* label = exportGraph ? L"export" : L"baseline";
    Check(api, api->EnableProfiling(options.ptr, label));
    if (exportGraph) {
        Check(api, api->AddSessionConfigEntry(options.ptr, "ep.dml.enable_graph_serialization", "true"));
    }

    OrtObject<OrtSession, &OrtApi::ReleaseSession> session(api);
    const auto start = Clock::now();
    Check(api, api->CreateSession(env, model.c_str(), options.ptr, &session.ptr));
    const auto ready = Clock::now();

    std::vector<float> input = MakeInput(0);
    constexpr std::array<int64_t, 4> shape{1, 3, Width, Width};
    OrtObject<OrtMemoryInfo, &OrtApi::ReleaseMemoryInfo> info(api);
    Check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &info.ptr));
    OrtObject<OrtValue, &OrtApi::ReleaseValue> tensor(api);
    Check(api, api->CreateTensorWithDataAsOrtValue(info.ptr, input.data(), input.size() * sizeof(float),
          shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tensor.ptr));

    const char* inputName = "image";
    const char* outputName = "depth";
    const OrtValue* inputValue = tensor.ptr;
    constexpr std::array<int64_t, 4> depthShape{1, 1, Width, Width};
    OrtAllocator* allocator = nullptr;
    Check(api, api->GetAllocatorWithDefaultOptions(&allocator));
    OrtObject<OrtValue, &OrtApi::ReleaseValue> output(api);
    Check(api, api->CreateTensorAsOrtValue(allocator, depthShape.data(), depthShape.size(),
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &output.ptr));
    Check(api, api->Run(session.ptr, nullptr, &inputName, &inputValue, 1, &outputName, 1, &output.ptr));

    OrtObject<OrtTensorTypeAndShapeInfo, &OrtApi::ReleaseTensorTypeAndShapeInfo> outputInfo(api);
    Check(api, api->GetTensorTypeAndShape(output.ptr, &outputInfo.ptr));
    ONNXTensorElementDataType elementType{};
    std::size_t dimensions = 0;
    Check(api, api->GetTensorElementType(outputInfo.ptr, &elementType));
    Check(api, api->GetDimensionsCount(outputInfo.ptr, &dimensions));
    std::array<int64_t, 4> outputShape{};
    if (dimensions != outputShape.size() || elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("Unexpected depth tensor rank or type");
    }
    Check(api, api->GetDimensions(outputInfo.ptr, outputShape.data(), outputShape.size()));
    if (outputShape != depthShape) {
        throw std::runtime_error("Unexpected depth tensor shape");
    }
    const OrtMemoryInfo* outputMemory = nullptr;
    Check(api, api->GetTensorMemoryInfo(output.ptr, &outputMemory));
    OrtMemoryInfoDeviceType outputDevice{};
    api->MemoryInfoGetDeviceType(outputMemory, &outputDevice);
    if (outputDevice != OrtMemoryInfoDeviceType_CPU) throw std::runtime_error("Depth tensor was not read back to the CPU");
    void* data = nullptr;
    Check(api, api->GetTensorMutableData(output.ptr, &data));
    const auto* begin = static_cast<const float*>(data);
    RunResult result;
    result.depth.assign(begin, begin + Pixels);
    if (!std::all_of(result.depth.begin(), result.depth.end(), [](float x) { return std::isfinite(x); })) {
        throw std::runtime_error("Non-finite depth tensor");
    }
    const auto [lo, hi] = std::minmax_element(result.depth.begin(), result.depth.end());
    if (*lo == *hi) throw std::runtime_error("Constant depth tensor");

    char* profileName = nullptr;
    Check(api, api->SessionEndProfiling(session.ptr, allocator, &profileName));
    struct ProfileOwner {
        OrtAllocator* allocator;
        char* name;
        ~ProfileOwner() { if (name) allocator->Free(allocator, name); }
    } profileOwner{allocator, profileName};
    if (!profileName || !*profileName) throw std::runtime_error("ORT returned no profiling file");
    result.profile = fs::path(profileName);
    if (!exportGraph) {
        for (int index = 0; index < InputCount; ++index) {
            const auto pattern = MakeInput(index);
            std::copy(pattern.begin(), pattern.end(), input.begin());
            Check(api, api->Run(session.ptr, nullptr, &inputName, &inputValue, 1, &outputName, 1, &output.ptr));
            Check(api, api->GetTensorMutableData(output.ptr, &data));
            std::ofstream ref("ort-depth-" + std::to_string(index) + ".f32", std::ios::binary);
            if (!ref.write(static_cast<const char*>(data), Pixels * sizeof(float))) {
                throw std::runtime_error("Could not save reference tensor");
            }
        }
        constexpr int samples = 15;
        std::array<double, samples> times{};
        for (auto& time : times) {
            const auto start = Clock::now();
            Check(api, api->Run(session.ptr, nullptr, &inputName, &inputValue, 1, &outputName, 1, &output.ptr));
            time = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }
        std::sort(times.begin(), times.end());
        std::wcout << L"ORT warm CPU-to-CPU median: " << times[samples / 2] << L" ms" << std::endl;
    }

    std::wcout << label << L": session " << std::chrono::duration<double, std::milli>(ready - start).count()
               << L" ms (profile enabled); depth range " << *lo << L" to " << *hi << std::endl;
    return result;
}

struct NodeEvent {
    std::string provider;
    std::string op;
    std::string name;
};

std::vector<NodeEvent> ComputeNodes(const fs::path& profile)
{
    std::ifstream input(profile);
    if (!input) throw std::runtime_error("Unable to open ORT profile");
    const auto events = Json::parse(input);
    if (!events.is_array()) throw std::runtime_error("Unexpected ORT profile structure");
    std::vector<NodeEvent> nodes;
    for (const auto& event : events) {
        if (event.value("cat", "") != "Node") continue;
        const auto name = event.value("name", "");
        if (!name.ends_with("_kernel_time")) continue;
        const auto& args = event.at("args");
        const auto op = args.value("op_name", "");
        if (op == "MemcpyFromHost" || op == "MemcpyToHost" || op == "Memcpy") continue;
        nodes.push_back({args.value("provider", ""), op, name});
    }
    return nodes;
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc > 3 || (argc > 1 && std::wstring(argv[1]) == L"--help")) {
            std::wcout << L"Usage: zipdepth-dml-export.exe [runtime-folder] [mixed-model.onnx]\n"
                          L"Default runtime folder: dist\\dlss\\zipdepth under the current directory.\n";
            return argc > 3 ? 1 : 0;
        }
        const auto runtime = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path(L"dist/dlss/zipdepth"));
        const auto model = argc > 2 ? fs::absolute(argv[2]) : runtime / L"zipdepth_edgepad_384_mixed.onnx";
        if (!fs::is_regular_file(model)) {
            std::wcout << L"Model missing: " << model << L'\n';
            return 1;
        }
        const auto report = fs::temp_directory_path() /
            (L"zipdepth-dml-check-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()));
        if (!fs::create_directory(report)) throw std::runtime_error("Report directory already exists");
        std::wcout << L"Model: " << model << L" (" << fs::file_size(model) << L" bytes)\n"
                   << L"Runtime folder: " << runtime << L'\n'
                   << L"Report directory: " << report.native() << std::endl;
        fs::current_path(report);
        const fs::path stagedModel = report / L"zipdepth_edgepad_384_mixed.onnx";
        fs::copy_file(model, stagedModel);

        const Library directml(runtime / L"DirectML.dll");
        const Library onnx(runtime / L"onnxruntime.dll");
        const auto* base = onnx.symbol<decltype(&OrtGetApiBase)>("OrtGetApiBase")();
        const auto* api = base->GetApi(ORT_API_VERSION);
        if (!api) throw std::runtime_error("Requires ONNX Runtime C API 24 (the Edition bundle is 1.24.4)");
        std::wcout << L"ORT: " << base->GetVersionString() << std::endl;
        const void* provider = nullptr;
        Check(api, api->GetExecutionProviderApi("DML", ORT_API_VERSION, &provider));
        if (!provider) throw std::runtime_error("DirectML execution provider unavailable");
        const auto* dml = static_cast<const OrtDmlApi*>(provider);

        const auto adapter = PickAdapter();
        DXGI_ADAPTER_DESC1 adapterDesc{};
        Check(adapter->GetDesc1(&adapterDesc), "GetDesc1");
        std::wcout << L"Adapter: " << adapterDesc.Description << std::endl;
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf())),
              "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ComPtr<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.GetAddressOf())), "CreateCommandQueue");
        constexpr GUID dmlDeviceIid{0x6dbd6437, 0x96fd, 0x423f, {0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f}};
        ComPtr<IDMLDevice> dmlDevice;
        const auto create = directml.symbol<decltype(&DMLCreateDevice)>("DMLCreateDevice");
        Check(create(device.Get(), DML_CREATE_DEVICE_FLAG_NONE, dmlDeviceIid,
                     reinterpret_cast<void**>(dmlDevice.GetAddressOf())), "DMLCreateDevice");

        OrtObject<OrtEnv, &OrtApi::ReleaseEnv> env(api);
        Check(api, api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "ZipDepth DML check", &env.ptr));
        const auto baseline = RunModel(api, dml, env.ptr, dmlDevice.Get(), queue.Get(), stagedModel, false);
        const auto exported = RunModel(api, dml, env.ptr, dmlDevice.Get(), queue.Get(), stagedModel, true);
        const auto baselineNodes = ComputeNodes(baseline.profile);
        const auto exportNodes = ComputeNodes(exported.profile);

        std::vector<fs::path> partitions;
        for (const auto& entry : fs::recursive_directory_iterator(report)) {
            const auto path = entry.path();
            if (entry.is_regular_file() && path.filename().wstring().starts_with(L"Partition_") &&
                path.extension() == L".bin" && entry.file_size() > 0) {
                partitions.push_back(path);
            }
        }
        std::wcout << L"Partition files: " << partitions.size() << std::endl;
        for (const auto& path : partitions) std::wcout << L"  " << path << std::endl;
        std::wcout << L"Profiled compute nodes, baseline/export: " << baselineNodes.size()
                  << '/' << exportNodes.size() << '\n';
        for (const auto& node : exportNodes) {
            std::wcout << L"  " << node.provider.c_str() << L"  " << node.op.c_str() << L"  " << node.name.c_str() << '\n';
        }
        float maxDifference = 0.0f;
        for (std::size_t i = 0; i < Pixels; ++i) {
            maxDifference = std::max(maxDifference, std::abs(baseline.depth[i] - exported.depth[i]));
        }
        std::wcout << L"Depth max absolute difference: " << std::setprecision(9) << maxDifference << '\n';
        const bool oneDmlNode = exportNodes.size() == 1 &&
            exportNodes.front().provider == "DmlExecutionProvider" &&
            exportNodes.front().op.starts_with("DmlFusedNode_");
        if (partitions.size() == 1 && oneDmlNode && maxDifference <= 0.001f) {
            std::wcout << L"RESULT: SINGLE_PARTITION_CANDIDATE\n";
            return 0;
        }
        std::wcout << L"RESULT: SINGLE_PARTITION_NOT_CONFIRMED\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\nRESULT: CHECK_FAILED\n";
        return 1;
    }
}
