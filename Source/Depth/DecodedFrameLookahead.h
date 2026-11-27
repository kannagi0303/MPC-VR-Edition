// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "Edition/PlaybackCancellation.h"

class CMpcVideoRenderer;

// Owns detached input samples. Presentation follows the existing DirectShow
// clock; a separate worker keeps depth fed from the oldest queued sample.
class DecodedFrameLookahead {
public:
    explicit DecodedFrameLookahead(CMpcVideoRenderer& renderer, unsigned durationMs, bool originalColor = false);
    ~DecodedFrameLookahead();
    using Ticket = mpcvr::edition::PlaybackCancellation::Ticket;
    bool Push(IMediaSample* sample, int64_t pts, uint64_t bytes, uint64_t epoch, Ticket ticket = {});
    bool OriginalColor() const noexcept { return m_originalColor; }
    // Borrow a reference to the actual next original; never wait for one.
    CComPtr<IMediaSample> NextOriginal(uint64_t epoch);
    bool Idle();
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
        Ticket ticket;
    };

    void Present();
    void Prefetch();

    CMpcVideoRenderer& m_renderer;
    const int64_t m_lead;
    const bool m_originalColor;
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::deque<Frame> m_frames;
    std::thread m_presenter, m_prefetcher;
    std::atomic<uint64_t> m_epoch = 1;
    uint64_t m_bytes = 0;
    bool m_rendering = false;
    bool m_stopping = false;
};
