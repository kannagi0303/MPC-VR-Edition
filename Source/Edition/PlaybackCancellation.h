// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

// Each source timeline owns a *permanent* cancel signal. Never ResetEvent on
// a token that an older Receive/GPU job may still be waiting on. A new film
// or seek gets a different token, so old work cannot resume after restart.
namespace mpcvr::edition {
class CancellationSignal final {
    std::atomic_bool cancelled_{false};
    HANDLE event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
public:
    CancellationSignal() = default;
    ~CancellationSignal() { if (event_) CloseHandle(event_); }
    CancellationSignal(const CancellationSignal&) = delete;
    CancellationSignal& operator=(const CancellationSignal&) = delete;
    void Cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
        if (event_) SetEvent(event_);
    }
    bool Cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    HANDLE Event() const noexcept { return event_; }
};

class PlaybackCancellation final {
public:
    using Ticket = std::shared_ptr<CancellationSignal>;
private:
    mutable std::mutex lock_;
    Ticket active_ = std::make_shared<CancellationSignal>();
public:
    Ticket Capture() const {
        std::lock_guard guard(lock_);
        return active_;
    }
    void Cancel() noexcept {
        // Serialize cancellation with Restart. Capturing then unlocking
        // before Cancel could cancel an obsolete generation while a new
        // generation becomes active in between.
        std::lock_guard guard(lock_);
        active_->Cancel();
    }
    void Restart() {
        auto next = std::make_shared<CancellationSignal>();
        Ticket old;
        {
            std::lock_guard guard(lock_);
            old = std::exchange(active_, std::move(next));
            // An old waiter is woken before a new ticket can be observed.
            old->Cancel();
        }
    }
};
}
