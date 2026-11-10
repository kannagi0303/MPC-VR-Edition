// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <algorithm>

namespace mfg {
// A single output bank's two independent GPU timelines:
//  - producerDone: D3D12 NGX/readback has completed writing the bank
//  - presenterRead: D3D11 has completed sampling the output from that bank
// These fence values belong to different fence objects. Never compare an
// input-ready (D3D11 -> D3D12) value against producerDone: signals on
// independent GPU queues can complete out of order.
class GpuBankTimeline {
    uint64_t producerDone_ = 0;
    uint64_t presenterRead_ = 0;
public:
    void ProducerSubmitted(uint64_t value) { producerDone_ = std::max(producerDone_, value); }
    void PresenterSubmitted(uint64_t value) { presenterRead_ = std::max(presenterRead_, value); }
    bool ProducerReady(uint64_t producerCompleted) const {
        return !producerDone_ || producerCompleted >= producerDone_;
    }
    uint64_t ProducerFenceValue() const { return producerDone_; }
    uint64_t PresenterFenceValue() const { return presenterRead_; }
};
} // namespace mfg
