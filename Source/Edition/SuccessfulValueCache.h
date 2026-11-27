// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <mutex>

namespace edition {
// Immutable successful values only. The expensive producer runs outside the
// lock; simultaneous misses may do duplicate work, never wait for a compiler.
template<class Value> class SuccessfulValueCache {
    std::mutex mutex_;
    Value value_{};
public:
    Value Find() {
        std::lock_guard lock(mutex_);
        return value_;
    }
    Value Publish(const Value& value) {
        std::lock_guard lock(mutex_);
        if (!value_ && value) value_ = value;
        return value_;
    }
};
}
