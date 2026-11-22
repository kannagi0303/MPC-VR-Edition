// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace mfg {
// Admission and the producer must agree on the same available bank. The
// serials in protectedGroups still own their bank-backed generated outputs.
// Check producer and D3D11 reader fences independently: they are different
// timelines and their numeric values must never be compared to each other.
struct BankOffer {
    uint64_t serial = 0;
    uint64_t producerFence = 0;
    uint64_t presenterFence = 0;
};
inline std::optional<unsigned> SelectReusableBank(
    std::span<const BankOffer> banks, uint64_t nextSerial,
    uint64_t producerCompleted, uint64_t presenterCompleted,
    bool requirePresenterReadComplete, std::span<const uint64_t> protectedGroups)
{
    if (banks.empty()) return {};
    const auto preferred = size_t(nextSerial % banks.size());
    for (size_t offset = 0; offset < banks.size(); ++offset) {
        const auto index = (preferred + offset) % banks.size();
        const auto& bank = banks[index];
        if (bank.producerFence && producerCompleted < bank.producerFence) continue;
        if (requirePresenterReadComplete && bank.presenterFence &&
            presenterCompleted < bank.presenterFence) continue;
        if (std::find(protectedGroups.begin(), protectedGroups.end(), bank.serial) !=
            protectedGroups.end()) continue;
        return static_cast<unsigned>(index);
    }
    return {};
}
} // namespace mfg
