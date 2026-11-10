// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Experimental diagnostic ONLY. Never modifies renderer timing or frame
// selection. Include from a Windows renderer translation unit after stdafx.
#include <cstdio>
#include <cstdint>
#include <mutex>

namespace mfg {
class CadenceTrace {
    std::mutex mutex_;
    FILE* file_ = nullptr;
    unsigned pending_ = 0;
    double qpcFrequency_ = 0.0;
public:
    CadenceTrace() {
        wchar_t path[1024]{};
        const DWORD len = GetEnvironmentVariableW(L"MPCVR_MFG_CADENCE_TRACE", path, _countof(path));
        if (len == 0 || len >= _countof(path)) return;
        if (_wfopen_s(&file_, path, L"wb") != 0 || !file_) return;
        qpcFrequency_ = GetPreciseTicksPerSecond();
        std::fprintf(file_, "event,qpc,qpc_frequency,source_pts_100ns,stream_time_100ns,serial,phase,"
                            "effective_multiplier,ngx_generated,due_qpc,precomposed,"
                            "lost_generated_total,result_hresult\n");
        std::fflush(file_);
    }
    ~CadenceTrace() { if (file_) std::fclose(file_); }
    CadenceTrace(const CadenceTrace&) = delete;
    CadenceTrace& operator=(const CadenceTrace&) = delete;
    bool Enabled() const { return file_ != nullptr; }
    void Record(const char* event, int64_t qpc, int64_t pts, int64_t stream,
                uint64_t serial, unsigned phase, unsigned effective,
                unsigned generated, int64_t due, bool prepared,
                uint64_t lostGenerated, long result) {
        if (!file_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::fprintf(file_, "%s,%lld,%.0f,%lld,%lld,%llu,%u,%u,%u,%lld,%u,%llu,%ld\n",
                     event, (long long)qpc, qpcFrequency_, (long long)pts, (long long)stream,
                     (unsigned long long)serial, phase, effective, generated,
                     (long long)due, prepared ? 1u : 0u,
                     (unsigned long long)lostGenerated, result);
        if (++pending_ >= 64) { std::fflush(file_); pending_ = 0; }
    }
};
inline CadenceTrace& SourceCadenceTrace() {
    static CadenceTrace instance;
    return instance;
}
} // namespace mfg
