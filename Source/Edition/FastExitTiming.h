// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>

// UFE-2: entry-first breadcrumbs around lifecycle/connection boundaries,
// with thresholded slow warnings on frame paths. No media titles or paths.
// Temporary diagnostics: remove or gate them after the block is isolated.
namespace mpcvr::exit_timing {
inline constexpr const char* Build = "FAST_EXIT_UFE5_DEFER_PREPARATION_20261123";

inline void Write(const char* kind, const char* stage, ULONGLONG elapsed = 0) noexcept
{
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    constexpr wchar_t suffix[] = L"MPC-VR-Edition-fast-exit.log";
    if (!n || n + (sizeof(suffix) / sizeof(wchar_t)) > MAX_PATH) return;
    std::memcpy(path + n, suffix, sizeof(suffix));

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[320]{};
    const int len = std::snprintf(line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu build=%s kind=%s stage=%s elapsed_ms=%llu\r\n",
        unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay),
        unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond),
        unsigned(now.wMilliseconds), static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()), Build, kind, stage,
        static_cast<unsigned long long>(elapsed));
    if (len > 0 && len < int(sizeof(line))) {
        DWORD written = 0;
        WriteFile(file, line, DWORD(len), &written, nullptr);
    }
    CloseHandle(file);
}

inline void Mark(const char* stage) noexcept { Write("checkpoint", stage); }
inline void Slow(const char* stage, ULONGLONG start, ULONGLONG thresholdMs = 120) noexcept
{
    const auto elapsed = GetTickCount64() - start;
    if (elapsed >= thresholdMs) Write("slow", stage, elapsed);
}

// For frame paths, write nothing unless the call actually takes too long.
// This lets us observe an unexpectedly slow Receive without synchronous
// open/write/close calls on every ordinary video frame.
class SlowScope final {
    const char* stage_;
    ULONGLONG started_;
    ULONGLONG threshold_;
public:
    explicit SlowScope(const char* stage, ULONGLONG thresholdMs = 250) noexcept
        : stage_(stage), started_(GetTickCount64()), threshold_(thresholdMs) {}
    ~SlowScope() noexcept { Slow(stage_, started_, threshold_); }
    SlowScope(const SlowScope&) = delete;
    SlowScope& operator=(const SlowScope&) = delete;
};

// Entry/exit guard for infrequent lifecycle and connection callbacks.
// Returns and exceptions cannot accidentally omit the matching exit marker.
class TraceScope final {
    const char* leave_;
    const char* slow_;
    ULONGLONG started_;
    ULONGLONG threshold_;
public:
    TraceScope(const char* enter, const char* leave, const char* slow,
               ULONGLONG thresholdMs = 120) noexcept
        : leave_(leave), slow_(slow), started_(GetTickCount64()), threshold_(thresholdMs) {
        Mark(enter);
    }
    ~TraceScope() noexcept { Mark(leave_); Slow(slow_, started_, threshold_); }
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
};
}
