// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "DepthPack.h"

// A build-time split of a previously validated native pack. Runtime views
// reference the two original buffers directly; no concatenation or graph edit.
namespace depthsplit {
using namespace DepthPack;
using View = std::span<const std::uint8_t>;
using PairId = std::array<std::uint8_t, 16>;
inline constexpr wchar_t WeightFile[] = L"depthpro2-s-fp16.dmlweights";
inline constexpr wchar_t GraphFileName[] = L"dp2-s-w4-352x192-fp16.dmlgraph";
inline constexpr std::array<std::uint8_t, 8> GraphMagic{'Z','D','G','R','A','P',1,0};
inline constexpr std::array<std::uint8_t, 8> PoolMagic{'Z','D','P','O','O','L',1,0};
inline constexpr size_t PrefixSize = 24;
static_assert(PrefixSize % Alignment == 0);
inline bool SplitGraph(View data) {
    return data.size() >= GraphMagic.size() && std::ranges::equal(data.first(GraphMagic.size()), GraphMagic);
}
inline View Payload(View data, const std::array<std::uint8_t,8>& magic) {
    if (data.size() <= PrefixSize || data.size() > Limit + PrefixSize || !std::ranges::equal(data.first(8),magic))
        throw std::runtime_error("Invalid split depth resource header");
    return data.subspan(PrefixSize);
}
inline Entries Open(View graph, View pool = {}) {
    if (!SplitGraph(graph)) {
        if (!pool.empty()) throw std::runtime_error("Unexpected split weight resource");
        return Read(graph);
    }
    auto local = Read(Payload(graph,GraphMagic));
    auto shared = ReadEntries(Payload(pool,PoolMagic),true);
    if (!std::ranges::equal(graph.subspan(8,16),pool.subspan(8,16)))
        throw std::runtime_error("Mismatched depth graph and weight pair");
    if (local.size() + shared.size() > MaxEntries)
        throw std::runtime_error("Too many split depth tensors");
    local.merge(shared);
    if (!shared.empty()) throw std::runtime_error("Duplicate split depth tensor");
    return local;
}
struct Parts { Entries graph, weights; };
inline Parts Partition(const Entries& source) {
    Parts parts;
    // The pinned ORT exporter stores the fixed image-shape split masks as
    // external constants. Keep those with the graph and its inline constants.
    for (const auto& [name, bytes] : source) {
        const bool graphConstant = name == GraphFile || name == "split_split_0.bin" || name == "split_split_1.bin";
        (graphConstant ? parts.graph : parts.weights).emplace(name, bytes);
    }
    if (parts.weights.empty()) throw std::runtime_error("No external depth weights");
    return parts;
}
inline void WritePart(std::ostream& out, const Entries& entries, const PairId& id, bool weights) {
    const auto& magic = weights ? PoolMagic : GraphMagic;
    out.write(reinterpret_cast<const char*>(magic.data()),magic.size());
    out.write(reinterpret_cast<const char*>(id.data()),id.size());
    Write(out,entries,weights);
}
}
