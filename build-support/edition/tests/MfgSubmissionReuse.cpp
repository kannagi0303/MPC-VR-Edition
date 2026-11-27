// SPDX-License-Identifier: GPL-3.0-or-later
// Actual Session.cpp fragments, with a command-recording stand-in; no GPU.
#undef NDEBUG
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <random>
#include <vector>
namespace {
using UINT = unsigned;
constexpr unsigned MaxGenerated = 5;
enum D3D12_RESOURCE_STATES { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
constexpr int D3D12_RESOURCE_BARRIER_TYPE_TRANSITION=0;
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES=~0u;
struct ID3D12Resource { int id; D3D12_RESOURCE_STATES state; };
struct D3D12_RESOURCE_BARRIER { int Type; struct { ID3D12Resource* pResource; UINT Subresource;
    D3D12_RESOURCE_STATES StateBefore,StateAfter; } Transition; };
using Event=std::array<int,5>;
struct ID3D12GraphicsCommandList {
    std::vector<Event> events; unsigned calls=0;
    void ResourceBarrier(UINT n,const D3D12_RESOURCE_BARRIER* b) {
        assert(n); ++calls;
        for(UINT i=0;i<n;++i) {
            auto t=b[i].Transition;
            assert(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && t.Subresource==~0u);
            assert(t.pResource->state==t.StateBefore);
            events.push_back({0,t.pResource->id,t.StateBefore,t.StateAfter,0}); t.pResource->state=t.StateAfter;
        }
    }
    void CopyBufferRegion(ID3D12Resource* dst,unsigned offset,ID3D12Resource* src,unsigned start,unsigned bytes) {
        assert(dst->state==D3D12_RESOURCE_STATE_COPY_DEST && src->state==D3D12_RESOURCE_STATE_COPY_SOURCE);
        assert(start==0 && bytes==4); events.push_back({1,dst->id,src->id,int(offset),int(bytes)});
    }
};
#include "MfgTransitionHelpers.generated.inc"
struct Ptr { ID3D12Resource* p; ID3D12Resource* Get() const {return p;} };
struct Shared { Ptr d12; };
struct Bank { Shared real,depth,motion; std::array<Shared,MaxGenerated> generated;
    std::array<Ptr,MaxGenerated> disabled; Ptr readback; };
struct Runtime {
    enum class Evaluation { Failed, LowerMultiplier, Accepted };
    Evaluation result;
    Evaluation Evaluate(ID3D12GraphicsCommandList* work,ID3D12Resource* real,ID3D12Resource* depth,
        ID3D12Resource* motion,const std::array<ID3D12Resource*,MaxGenerated>& out,
        const std::array<ID3D12Resource*,MaxGenerated>& flag,unsigned mult,bool reset) {
        for(auto p:{real,depth,motion}) assert(p->state==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=0;i<mult-1;++i) {
            assert(out[i]->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            assert(flag[i]->state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        work->events.push_back({2,int(mult),int(reset),int(result),0}); return result;
    }
};
void Verify(unsigned multiplier,bool reset,bool hasFlow,Runtime::Evaluation result) {
    ID3D12Resource real{1,D3D12_RESOURCE_STATE_COMMON},depth{2,D3D12_RESOURCE_STATE_COMMON},motion{3,D3D12_RESOURCE_STATE_COMMON};
    ID3D12Resource zeros{4,D3D12_RESOURCE_STATE_COPY_SOURCE},readback{5,D3D12_RESOURCE_STATE_COPY_DEST};
    std::array<ID3D12Resource,MaxGenerated> outputs{},flags{};
    Bank bank{{{&real}},{{&depth}},{{&motion}},{},{},{&readback}};
    for(unsigned i=0;i<MaxGenerated;++i) {
        outputs[i]={int(10+i),D3D12_RESOURCE_STATE_COMMON}; flags[i]={int(20+i),D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        bank.generated[i].d12={&outputs[i]}; bank.disabled[i]={&flags[i]};
    }
    Ptr zero{&zeros}; Runtime runtime{result}; ID3D12GraphicsCommandList list; auto* work=&list;
#include "MfgTransitionCommands.generated.inc"
    assert(evaluation==result);
    std::vector<Event> expected;
    auto barrier=[&](int id,int a,int b){expected.push_back({0,id,a,b,0});};
    for(int id:{1,2,3}) barrier(id,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for(unsigned i=0;i<multiplier-1;++i) {
        const int outId=10+i,flagId=20+i;
        barrier(outId,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(flagId,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
        expected.push_back({1,flagId,4,0,4});
        barrier(flagId,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    expected.push_back({2,int(multiplier),int(reset||!hasFlow),int(result),0});
    for(unsigned i=0;i<multiplier-1;++i) {
        const int outId=10+i,flagId=20+i;
        barrier(outId,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        barrier(flagId,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        expected.push_back({1,5,flagId,int(i*4),4});
        barrier(flagId,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    for(int id:{1,2,3}) barrier(id,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    assert(list.events==expected);
    assert(list.calls==2+4*(multiplier-1));
    for(auto p:{&real,&depth,&motion}) assert(p->state==D3D12_RESOURCE_STATE_COMMON);
    for(unsigned i=0;i<MaxGenerated;++i) {
        assert(outputs[i].state==D3D12_RESOURCE_STATE_COMMON);
        assert(flags[i].state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}
struct ConstantBuffer { std::array<int,12> payload{}; unsigned writes=0; };
struct ConstantContext {
    void UpdateSubresource(ConstantBuffer* dest,unsigned sub,void* box,const void* data,unsigned row,unsigned depth) {
        assert(sub==0&&!box&&!row&&!depth); std::memcpy(dest->payload.data(),data,48); ++dest->writes;
    }
};
struct ConstantSession {
    ConstantBuffer buffer; ConstantContext store; ConstantContext* context=&store;
    struct {ConstantBuffer* p; ConstantBuffer* Get() {return p;}} constants{&buffer};
#include "MfgCopyConstants.generated.inc"
};
}
void TestMfgSubmissionReuse() {
    unsigned cases=0;
    for(unsigned mult=2;mult<=6;++mult) for(bool reset:{false,true}) for(bool flow:{false,true})
        for(auto result:{Runtime::Evaluation::Failed,Runtime::Evaluation::LowerMultiplier,Runtime::Evaluation::Accepted}) {
            Verify(mult,reset,flow,result); ++cases;
        }
    std::mt19937 rng(0x530);
    unsigned uploads=0;
    for(unsigned session=0;session<64;++session) {
        ConstantSession current;
        std::array<int,12> expected{};
        unsigned count=0;
        for(unsigned i=0;i<4096;++i) {
            auto next=expected;
            if(i%5==1) next[(i/5)%12]=int(rng()%100000);
            if(i%5==2) next={};
            const bool changed=i==0||next!=expected;
            current.UpdateCopyConstants(next);
            count+=changed; expected=next;
            assert(current.buffer.writes==count && current.buffer.payload==expected);
        }
        uploads+=count;
    }
    std::cout<<"PASS: "<<cases<<" transition traces; 262144 constant payloads, "<<uploads<<" actual uploads\n";
}
#ifdef MPCVR_STANDALONE_TEST
int main(){TestMfgSubmissionReuse();}
#endif
