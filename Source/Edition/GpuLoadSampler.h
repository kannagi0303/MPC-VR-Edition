#pragma once

// Optional NVIDIA GPU telemetry. NVML is dynamically loaded from the driver.
// All NVML calls run off the Present thread. The renderer never waits on the
// telemetry thread at shutdown; that thread pins this .ax DLL until exit.

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace edition {

struct GpuLoadSnapshot {
    int gpu = -1;        // GPU core activity over NVML sampling period
    int memory = -1;     // memory controller activity, NOT VRAM capacity
    int decoder = -1;
    int encoder = -1;
    int opticalFlow = -1;
    int idle = -1;       // sampled core idle time; NOT frame deadline margin
    int idleLow = -1;    // lowest raw idle observation in recent samples
};

class GpuLoadSampler final {
    struct Rates { unsigned int gpu, memory; };
    using InitFn = int (__cdecl*)();
    using CountFn = int (__cdecl*)(unsigned int*);
    using DeviceFn = int (__cdecl*)(unsigned int, void**);
    using RatesFn = int (__cdecl*)(void*, Rates*);
    using UnitFn = int (__cdecl*)(void*, unsigned int*, unsigned int*);
    using ShutdownFn = int (__cdecl*)();

    struct State {
        std::atomic<unsigned long long> lastObserver{0};
        std::atomic<int> gpu{-1}, memory{-1}, decoder{-1}, encoder{-1}, ofa{-1};
        std::atomic<int> idle{-1}, idleLow{-1};
        std::atomic<unsigned long long> sampleTick{0};
        std::mutex sleepMutex;
        std::condition_variable wake;
        bool stop = false; // protected by sleepMutex

        static int Smooth(int previous, int next) noexcept {
            return previous < 0 ? next : (previous * 3 + next + 2) / 4;
        }
        static void StoreSmoothed(std::atomic<int>& output, int next) noexcept {
            if (next < 0 || next > 100) output.store(-1, std::memory_order_relaxed);
            else output.store(Smooth(output.load(std::memory_order_relaxed), next), std::memory_order_relaxed);
        }
        void Stop() noexcept {
            {
                std::lock_guard guard(sleepMutex);
                stop = true;
            }
            wake.notify_one();
        }
        GpuLoadSnapshot Read() noexcept {
            const auto now = GetTickCount64();
            lastObserver.store(now, std::memory_order_relaxed);
            GpuLoadSnapshot s;
            const auto tick = sampleTick.load(std::memory_order_acquire);
            if (!tick || now - tick > 2600) return s;
            s.gpu = gpu.load(std::memory_order_relaxed);
            s.memory = memory.load(std::memory_order_relaxed);
            s.decoder = decoder.load(std::memory_order_relaxed);
            s.encoder = encoder.load(std::memory_order_relaxed);
            s.opticalFlow = ofa.load(std::memory_order_relaxed);
            s.idle = idle.load(std::memory_order_relaxed);
            s.idleLow = idleLow.load(std::memory_order_relaxed);
            return s;
        }
        void Run() noexcept {
            // Only one NVIDIA GPU: no arbitrary first-GPU choice on multi-GPU.
            HMODULE nvml = LoadLibraryW(L"nvml.dll");
            if (!nvml) return;
            const auto get = [nvml](const char* symbol) { return GetProcAddress(nvml, symbol); };
            const auto init = reinterpret_cast<InitFn>(get("nvmlInit_v2"));
            const auto shutdown = reinterpret_cast<ShutdownFn>(get("nvmlShutdown"));
            const auto count = reinterpret_cast<CountFn>(get("nvmlDeviceGetCount_v2"));
            const auto handle = reinterpret_cast<DeviceFn>(get("nvmlDeviceGetHandleByIndex_v2"));
            const auto utilization = reinterpret_cast<RatesFn>(get("nvmlDeviceGetUtilizationRates"));
            const auto decode = reinterpret_cast<UnitFn>(get("nvmlDeviceGetDecoderUtilization"));
            const auto encode = reinterpret_cast<UnitFn>(get("nvmlDeviceGetEncoderUtilization"));
            const auto flow = reinterpret_cast<UnitFn>(get("nvmlDeviceGetOfaUtilization"));
            if (!init || !shutdown || !count || !handle || !utilization || init() != 0) {
                FreeLibrary(nvml);
                return;
            }
            unsigned int devices = 0;
            void* device = nullptr;
            if (count(&devices) != 0 || devices != 1 || handle(0, &device) != 0 || !device) {
                shutdown();
                FreeLibrary(nvml);
                return;
            }
            std::array<int, 6> idleHistory{};
            unsigned historyCount = 0, historyPos = 0;
            for (;;) {
                {
                    std::unique_lock lock(sleepMutex);
                    wake.wait_for(lock, std::chrono::milliseconds(900), [this] { return stop; });
                    if (stop) break;
                }
                const auto now = GetTickCount64();
                if (now - lastObserver.load(std::memory_order_relaxed) > 2400) continue;
                Rates busy{};
                if (utilization(device, &busy) == 0 && busy.gpu <= 100 && busy.memory <= 100) {
                    StoreSmoothed(gpu, static_cast<int>(busy.gpu));
                    StoreSmoothed(memory, static_cast<int>(busy.memory));
                    const int freeTime = 100 - static_cast<int>(busy.gpu);
                    StoreSmoothed(idle, freeTime);
                    idleHistory[historyPos] = freeTime;
                    historyPos = (historyPos + 1) % static_cast<unsigned>(idleHistory.size());
                    historyCount = (std::min)(historyCount + 1, static_cast<unsigned>(idleHistory.size()));
                    idleLow.store(*std::min_element(idleHistory.begin(), idleHistory.begin() + historyCount),
                        std::memory_order_relaxed);
                    sampleTick.store(now, std::memory_order_release);
                } else {
                    gpu.store(-1, std::memory_order_relaxed);
                    memory.store(-1, std::memory_order_relaxed);
                    idle.store(-1, std::memory_order_relaxed);
                    idleLow.store(-1, std::memory_order_relaxed);
                    sampleTick.store(0, std::memory_order_release);
                    historyCount = historyPos = 0;
                }
                const auto sampleUnit = [device](UnitFn fn, std::atomic<int>& dst) {
                    unsigned int rate = 0, period = 0;
                    StoreSmoothed(dst, fn && fn(device, &rate, &period) == 0 && rate <= 100 ?
                        static_cast<int>(rate) : -1);
                };
                sampleUnit(decode, decoder);
                sampleUnit(encode, encoder);
                sampleUnit(flow, ofa);
            }
            shutdown();
            FreeLibrary(nvml);
        }
    };

    struct WorkerContext {
        std::shared_ptr<State> state;
        HMODULE pin = nullptr;
    };
    static DWORD WINAPI ThreadMain(void* param) noexcept {
        // Retain the module while executing code from its .ax. End the thread
        // atomically with FreeLibrary so no instruction executes in an
        // already-unloaded DLL, even if MPC-BE switches media immediately.
        std::unique_ptr<WorkerContext> worker(static_cast<WorkerContext*>(param));
        worker->state->Run();
        const HMODULE pinnedModule = worker->pin;
        worker.reset(); // Release all objects before unloading module code.
        FreeLibraryAndExitThread(pinnedModule, 0);
        return 0; // not reached
    }

    std::shared_ptr<State> state_;

public:
    GpuLoadSampler() : state_(std::make_shared<State>()) {
        HMODULE pin = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&GpuLoadSampler::ThreadMain), &pin)) return;
        auto worker = std::make_unique<WorkerContext>();
        worker->state = state_;
        worker->pin = pin;
        HANDLE thread = CreateThread(nullptr, 0, &GpuLoadSampler::ThreadMain, worker.get(), 0, nullptr);
        if (!thread) {
            FreeLibrary(pin);
            return;
        }
        worker.release();
        CloseHandle(thread);
    }
    GpuLoadSampler(const GpuLoadSampler&) = delete;
    GpuLoadSampler& operator=(const GpuLoadSampler&) = delete;
    ~GpuLoadSampler() {
        // No join/driver call from renderer exit. Worker has its own State ref
        // and holds the .ax module reference until NVML calls finish.
        state_->Stop();
    }
    GpuLoadSnapshot Read() noexcept { return state_->Read(); }
};

} // namespace edition
