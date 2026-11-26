// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cassert>

namespace depthjobs {
// All operations are protected by the worker's existing mutex. The producer
// is the serialized renderer context; it may reserve a capture bank while
// the worker completes the other bank. No asynchronous producer reservations.
template<class Result> struct Queue {
    struct Job { Result frame{}; std::uint64_t serial = 0; unsigned captureBank = 0; };
    static constexpr unsigned MaxOutstanding = 3;
    Job pendingJob{}, runningJob{};
    bool pending = false, running = false;
    std::array<Result, MaxOutstanding> results{};
    unsigned count = 0;

    bool CanAccept() const {
        return !pending && count + unsigned(running) < MaxOutstanding;
    }
    unsigned CaptureBank() const {
        return running ? 1u - runningJob.captureBank : 0u;
    }
    bool SlotBusy(unsigned slot) const {
        if ((pending && pendingJob.frame.slot == slot) || (running && runningJob.frame.slot == slot)) return true;
        for (unsigned i = 0; i < count; ++i) if (results[i].slot == slot) return true;
        return false;
    }
    bool CanSubmit(unsigned bank, unsigned slot) const {
        // The worker may finish after the producer chose a bank. Either bank
        // is then safe; it cannot start another job before this submit.
        return CanAccept() && bank < 2u &&
            (!running || bank != runningJob.captureBank) && !SlotBusy(slot);
    }
    void Submit(Job job) { assert(!pending); pendingJob = job; pending = true; }
    Job Begin() { assert(pending && !running); runningJob = pendingJob; pending = false; running = true; return runningJob; }
    void Complete(Result result) {
        assert(running && result.slot == runningJob.frame.slot && count < MaxOutstanding);
        results[count++] = result; running = false;
    }
    bool Take(Result& result) {
        if (!count) return false;
        result = results[0];
        for (unsigned i = 1; i < count; ++i) results[i-1] = results[i];
        --count;
        return true;
    }
    void CancelPending() { pending = false; }
};
}
