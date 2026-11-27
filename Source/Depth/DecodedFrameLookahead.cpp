// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "DecodedFrameLookahead.h"
#include "VideoRenderer.h"
#include <chrono>
#include <new>

namespace {
constexpr uint64_t MaxVideoBytes = 1024ull * 1024 * 1024;
struct ComThread {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComThread() { if (SUCCEEDED(initialized)) CoUninitialize(); }
};
}

DecodedFrameLookahead::DecodedFrameLookahead(CMpcVideoRenderer& renderer, unsigned durationMs, bool originalColor)
    : m_renderer(renderer), m_lead(int64_t(durationMs) * 10'000), m_originalColor(originalColor)
{
    try {
        m_presenter = std::thread([this] { Present(); });
        if (!m_originalColor) m_prefetcher = std::thread([this] { Prefetch(); });
    } catch (...) {
        Shutdown();
        throw;
    }
}

DecodedFrameLookahead::~DecodedFrameLookahead() { Shutdown(); }

bool DecodedFrameLookahead::Push(IMediaSample* sample, int64_t pts, uint64_t bytes, uint64_t epoch, Ticket ticket)
{
    std::unique_lock lock(m_lock);
    const auto canPush = [&] {
        // Color staging has one pending original in addition to the active
        // producer. It does not require a particular lead, cadence or FPS.
        return m_stopping || Epoch() != epoch || (ticket && ticket->Cancelled()) || m_frames.empty() ||
            (!m_originalColor && m_bytes + bytes <= MaxVideoBytes && pts - m_frames.front().pts <= m_lead);
    };
    if (!canPush()) {
        m_changed.wait(lock, canPush);
    }
    if (m_stopping || Epoch() != epoch || (ticket && ticket->Cancelled())) return false;
    m_frames.push_back({sample, pts, bytes, epoch, false, std::move(ticket)});
    m_bytes += bytes;
    m_changed.notify_all();
    return true;
}

CComPtr<IMediaSample> DecodedFrameLookahead::NextOriginal(uint64_t epoch)
{
    std::lock_guard lock(m_lock);
    if (!m_originalColor || m_stopping || Epoch() != epoch || m_frames.empty()) return {};
    const auto& next = m_frames.front();
    if (next.epoch != epoch || !next.ticket || next.ticket->Cancelled()) return {};
    return next.sample;
}

bool DecodedFrameLookahead::Idle()
{
    std::lock_guard lock(m_lock);
    return m_frames.empty() && !m_rendering;
}

void DecodedFrameLookahead::Drain()
{
    std::unique_lock lock(m_lock);
    const auto epoch = Epoch();
    m_changed.wait(lock, [&] {
        return m_stopping || Epoch() != epoch || (m_frames.empty() && !m_rendering);
    });
}

void DecodedFrameLookahead::Cancel()
{
    if (m_originalColor) {
        // One pending original permits an allocation-free cancellation.
        // Release the decoder lease after publishing the new epoch and
        // after releasing the queue mutex, including Stop/Flush paths.
        Frame retired{};
        {
            std::lock_guard lock(m_lock);
            ++m_epoch;
            if (!m_frames.empty()) {
                retired = std::move(m_frames.front());
                m_frames.pop_front();
            }
            m_bytes = 0;
        }
        m_changed.notify_all();
        return;
    }
    std::deque<Frame> retired;
    {
        std::lock_guard lock(m_lock);
        ++m_epoch;
        // Release decoder-owned sample/texture COM references *outside* the
        // queue mutex. Destructors can wait for device/allocator work; holding
        // m_lock would stop other waiting threads from observing cancellation.
        retired.swap(m_frames);
        m_bytes = 0;
    }
    m_changed.notify_all();
    if (!retired.empty()) {
        retired.clear();
    }
}

void DecodedFrameLookahead::Shutdown()
{
    {
        std::lock_guard lock(m_lock);
        m_stopping = true;
    }
    Cancel();
    if (m_prefetcher.joinable()) m_prefetcher.join();
    if (m_presenter.joinable()) m_presenter.join();
}

void DecodedFrameLookahead::Present()
{
    const ComThread com;
    for (;;) {
        Frame frame{};
        {
            std::unique_lock lock(m_lock);
            m_changed.wait(lock, [&] { return m_stopping || !m_frames.empty(); });
            if (m_stopping) return;
            frame = std::move(m_frames.front());
            m_frames.pop_front();
            m_bytes -= frame.bytes;
            m_rendering = true;
            m_changed.notify_all();
        }
        // Both modes share ownership and cancellation, but color originals
        // MUST go through ordinary Completed admission, never a second clock.
        HRESULT hr;
        if (m_originalColor) {
            try {
                hr = m_renderer.ReceiveLookaheadOriginal(frame.sample, frame.epoch, frame.ticket);
            } catch (const std::bad_alloc&) {
                hr = E_OUTOFMEMORY;
            } catch (...) {
                hr = E_FAIL;
            }
        } else hr = m_renderer.ReceiveScheduled(frame.sample, frame.epoch);
        if (FAILED(hr) && Epoch() == frame.epoch) m_renderer.ReportLookaheadError(hr, frame.epoch);
        {
            std::lock_guard lock(m_lock);
            m_rendering = false;
            m_changed.notify_all();
        }
    }
}

void DecodedFrameLookahead::Prefetch()
{
    const ComThread com;
    for (;;) {
        Frame frame{};
        {
            std::unique_lock lock(m_lock);
            auto next = [&] {
                return std::find_if(m_frames.begin(), m_frames.end(), [](const Frame& f) { return !f.depthSubmitted; });
            };
            m_changed.wait(lock, [&] { return m_stopping || next() != m_frames.end(); });
            if (m_stopping) return;
            frame = *next();
        }
        const bool done = m_renderer.PrefetchDepth(frame.sample, frame.pts, frame.epoch);
        std::unique_lock lock(m_lock);
        if (done) {
            const auto frameIt = std::find_if(m_frames.begin(), m_frames.end(), [&](const Frame& candidate) {
                return candidate.sample.p == frame.sample.p;
            });
            if (frameIt != m_frames.end()) frameIt->depthSubmitted = true;
        } else {
            m_changed.wait_for(lock, std::chrono::milliseconds(2));
        }
    }
}
