// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class CMpcVideoRenderer;

// Owns detached input samples. Presentation follows the existing DirectShow
// clock; a separate worker keeps depth fed from the oldest queued sample.
class DecodedFrameLookahead {
public:
    explicit DecodedFrameLookahead(CMpcVideoRenderer& renderer, unsigned durationMs);
    ~DecodedFrameLookahead();
    bool Push(IMediaSample* sample, int64_t pts, uint64_t bytes, uint64_t epoch);
    void Drain();
    void Cancel();
    void Shutdown();
    uint64_t Epoch() const { return m_epoch.load(); }

private:
    struct Frame {
        CComPtr<IMediaSample> sample;
        int64_t pts;
        uint64_t bytes;
        uint64_t epoch;
        bool depthSubmitted = false;
    };

    void Present();
    void Prefetch();

    CMpcVideoRenderer& m_renderer;
    const int64_t m_lead;
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::deque<Frame> m_frames;
    std::thread m_presenter, m_prefetcher;
    std::atomic<uint64_t> m_epoch = 1;
    uint64_t m_bytes = 0;
    bool m_rendering = false;
    bool m_stopping = false;
};
