// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Opt-in diagnostic. Include from a Windows renderer translation unit after stdafx.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>

namespace mfg {
class CadenceTrace {
public:
    struct CompletedQueue {
        std::size_t frames = 0;
        std::size_t batches = 0;
        int64_t tailDue = -1;
        double leadBeforeMs = 0;
        double leadAfterMs = 0;
        std::size_t pacingSamples = 0;
    };

private:
    struct Entry {
        const char* event = nullptr;
        int64_t qpc = 0, pts = 0, stream = 0;
        uint64_t serial = 0;
        unsigned phase = 0, effective = 0, generated = 0;
        int64_t due = 0;
        bool prepared = false;
        uint64_t lostGenerated = 0;
        long result = 0;
        uint64_t duration = 0;
        int64_t presentationTime = -1;
        bool hasCompleted = false;
        CompletedQueue completed;
    };

    struct State {
        std::mutex lock;
        std::array<Entry, 2048> queue;
        std::size_t head = 0, count = 0;
        std::atomic<bool> scheduled{false}, failed{false};
        std::atomic<uint64_t> dropped{0};
        std::array<wchar_t, 1024> path{};
        FILE* file = nullptr;
        double frequency = 0;
    };

    State* state_ = nullptr;

    static void Write(FILE* file, double frequency, const Entry& entry)
    {
        std::fprintf(file, "%s,%lld,%.0f,%lld,%lld,%llu,%u,%u,%u,%lld,%u,%llu,%ld,%llu,%lld",
            entry.event, (long long)entry.qpc, frequency, (long long)entry.pts, (long long)entry.stream,
            (unsigned long long)entry.serial, entry.phase, entry.effective, entry.generated,
            (long long)entry.due, entry.prepared ? 1u : 0u, (unsigned long long)entry.lostGenerated,
            entry.result, (unsigned long long)entry.duration, (long long)entry.presentationTime);
        if (entry.hasCompleted) {
            const auto& queue = entry.completed;
            std::fprintf(file, ",%zu,%zu,%lld,%.3f,%.3f,%zu\n", queue.frames, queue.batches,
                (long long)queue.tailDue, queue.leadBeforeMs, queue.leadAfterMs, queue.pacingSamples);
        } else {
            std::fputs(",,,,,,\n", file);
        }
    }

    static void Drain(State& state)
    {
        if (!state.file) {
            if (_wfopen_s(&state.file, state.path.data(), L"wb") != 0 || !state.file) {
                state.failed.store(true, std::memory_order_relaxed);
                return;
            }
            std::fputs("event,qpc,qpc_frequency,source_pts_100ns,stream_time_100ns,serial,phase,"
                "effective_multiplier,ngx_generated,due_qpc,precomposed,"
                "lost_generated_total,result_hresult,duration_qpc,presentation_time_100ns,"
                "completed_frames,completed_batches,completed_tail_due_100ns,"
                "submit_lead_before_ms,submit_lead_after_ms,submit_pacing_samples\n", state.file);
        }

        std::array<Entry, 64> entries;
        for (;;) {
            std::size_t count;
            {
                std::lock_guard guard(state.lock);
                count = (std::min)(entries.size(), state.count);
                if (!count) {
                    state.scheduled.store(false, std::memory_order_relaxed);
                    return;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    entries[i] = state.queue[state.head];
                    state.head = (state.head + 1) % state.queue.size();
                }
                state.count -= count;
            }
            for (std::size_t i = 0; i < count; ++i) Write(state.file, state.frequency, entries[i]);
            if (const auto dropped = state.dropped.exchange(0, std::memory_order_relaxed)) {
                Entry gap;
                gap.event = "trace_dropped";
                gap.qpc = static_cast<int64_t>(GetPreciseTick());
                gap.pts = gap.stream = -1;
                gap.serial = dropped;
                Write(state.file, state.frequency, gap);
            }
            if (std::fflush(state.file) || std::ferror(state.file)) {
                state.failed.store(true, std::memory_order_relaxed);
                std::fclose(state.file);
                return;
            }
        }
    }

    static void CALLBACK FlushOnWorker(PTP_CALLBACK_INSTANCE, void* context)
    {
        const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
        Drain(*static_cast<State*>(context));
        if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }

public:
    CadenceTrace()
    {
        std::array<wchar_t, 1024> path{};
        const DWORD len = GetEnvironmentVariableW(L"MPCVR_MFG_CADENCE_TRACE", path.data(),
            static_cast<DWORD>(path.size()));
        if (len == 0 || len >= static_cast<DWORD>(path.size())) return;
        auto* const state = new (std::nothrow) State{};
        if (!state) return;
        // The opt-in trace owns one process-lifetime queue. Pin this DLL so
        // an outstanding worker can drain after the player releases the filter.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&FlushOnWorker), &module)) {
            delete state;
            return;
        }
        state->frequency = GetPreciseTicksPerSecond();
        state->path = path;
        state_ = state;
    }
    CadenceTrace(const CadenceTrace&) = delete;
    CadenceTrace& operator=(const CadenceTrace&) = delete;
    bool Enabled() const { return state_ && !state_->failed.load(std::memory_order_relaxed); }

    // Event names are string literals. A full or busy queue drops diagnostics
    // without blocking the renderer or formatting any CSV on the hot path.
    void Record(const char* event, int64_t qpc, int64_t pts, int64_t stream,
                uint64_t serial, unsigned phase, unsigned effective,
                unsigned generated, int64_t due, bool prepared,
                uint64_t lostGenerated, long result, uint64_t duration = 0, int64_t presentationTime = -1,
                const CompletedQueue* completed = nullptr)
    {
        if (!Enabled()) return;
        auto& state = *state_;
        bool schedule;
        {
            std::unique_lock guard(state.lock, std::try_to_lock);
            if (!guard || state.count == state.queue.size()) {
                state.dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto& item = state.queue[(state.head + state.count) % state.queue.size()];
            item = {event, qpc, pts, stream, serial, phase, effective, generated, due, prepared,
                lostGenerated, result, duration, presentationTime, completed != nullptr,
                completed ? *completed : CompletedQueue{}};
            ++state.count;
            schedule = !state.scheduled.exchange(true, std::memory_order_relaxed);
        }
        if (schedule && !TrySubmitThreadpoolCallback(&FlushOnWorker, &state, nullptr)) {
            state.scheduled.store(false, std::memory_order_relaxed);
        }
    }
};

inline CadenceTrace& SourceCadenceTrace() {
    static CadenceTrace instance;
    return instance;
}
} // namespace mfg
