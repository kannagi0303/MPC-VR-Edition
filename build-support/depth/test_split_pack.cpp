// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../Source/Depth/DepthSplitPack.h"
#include <cassert>
#include <sstream>
#include <iostream>
#include <random>
using namespace DepthPack;
using namespace depthsplit;

Bytes Serialize(const Entries& entries, bool weights, PairId id = {1}) {
    std::ostringstream out(std::ios::binary);
    WritePart(out,entries,id,weights);
    const auto s=out.str();
    return Bytes(s.begin(),s.end());
}
template<class F> void Reject(F f) {
    bool rejected=false;
    try { f(); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected);
}
int main() {
    const Bytes graph{1,2,3,4,5,6,7,8}, a{10,11,12,13}, b{20,21,22,23}, c{31,32,33,34};
    const Entries src{{GraphFile,graph},{"tensor_a.bin",a},{"tensor_b.bin",b},{"split_split_0.bin",c}};
    const auto parts=Partition(src);
    assert(parts.graph.size()==2 && parts.graph.contains("split_split_0.bin") && parts.weights.size()==2);
    const auto g=Serialize(parts.graph,false), w=Serialize(parts.weights,true);
    assert(SplitGraph(g));
    const auto merged=Open(g,w);
    assert(merged.size()==src.size());
    for (const auto& [name,data] : src) assert(std::ranges::equal(merged.at(name),data));
    for (const auto& [name,data] : merged) {
        const auto& backing=parts.weights.contains(name)?w:g;
        assert(data.data() >= backing.data() && data.data()+data.size() <= backing.data()+backing.size());
        assert(reinterpret_cast<std::uintptr_t>(data.data()) % Alignment == 0);
    }
    Reject([&]{Open(g,{});});
    Reject([&]{Open(g,Serialize(parts.weights,true,{2}));});
    const auto graphWithDuplicate = Serialize(Entries{{GraphFile,graph},{"tensor_a.bin",a}},false);
    Reject([&]{Open(graphWithDuplicate,w);});
    Reject([&]{Open(g,g);});
    Reject([&]{Serialize(Entries{{GraphFile,graph}},true);});
    for(size_t i=0;i<g.size();++i) Reject([&]{Open(View(g).first(i),w);});
    for(size_t i=0;i<w.size();++i) Reject([&]{Open(g,View(w).first(i));});
    // The fixed native conversion intermediate retains the original version.
    std::ostringstream out(std::ios::binary); Write(out,src);
    const auto legacyString=out.str(); const Bytes legacy(legacyString.begin(),legacyString.end());
    assert(!SplitGraph(legacy) && Open(legacy).size()==src.size());
    Reject([&]{Open(legacy,w);});
    auto retiredShape = legacy;
    retiredShape[6] = 4;
    Reject([&]{Open(retiredShape);});
    // Distinct payloads with similar sizes/names, including renamed identical
    // content, must be preserved byte-for-byte under randomized partitioning.
    std::mt19937 rng(352192);
    for (unsigned trial=0;trial<1000;++trial) {
        std::array<Bytes,32> payloads;
        Entries input{{GraphFile,graph}};
        for(unsigned i=0;i<payloads.size();++i) {
            auto& v=payloads[i];v.resize(4*(1+rng()%64));
            for(auto& byte:v)byte=static_cast<uint8_t>(rng());
            input.emplace("t"+std::to_string(i)+".bin",v);
        }
        auto p=Partition(input);
        auto x=Serialize(p.graph,false),y=Serialize(p.weights,true);
        auto r=Open(x,y);
        assert(r.size()==input.size());
        for(const auto& [name,data]:input) assert(std::ranges::equal(r.at(name),data));
    }
    std::cout<<"PASS: split pack, 1000 randomized byte-identical round trips, direct spans, alignment, pairing, truncation, duplicate rejection and fixed intermediate compatibility\n";
}
