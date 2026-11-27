// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../Source/Edition/SuccessfulValueCache.h"
#include "../../../Source/Edition/SvdLutInput.h"
#include "../../../Source/Edition/SvdLutB2Refined.h"
#undef NDEBUG
#include <cassert>
#include <atomic>
#include <bit>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {
namespace in = svdlut::input;
namespace b2 = svdlut::b2::refined;
// FIX27's signature implementation, independently preserved for parity.
static in::Signature BaselineSignature(const uint8_t* data, size_t stride) {
    in::Signature signature{};
    constexpr unsigned block = in::Size / 8;
    constexpr unsigned step = 4;
    constexpr float normalized = 1.0f / ((block / step) * (block / step));
    for (unsigned y = 0; y < in::Size; y += step)
        for (unsigned x = 0; x < in::Size; x += step) {
            const auto tile = (y / block) * 8 + x / block;
            for (unsigned c = 0; c < 3; ++c)
                signature[tile * 3 + c] += in::Code(data,stride,x+step/2,y+step/2,c)*normalized;
        }
    return signature;
}
static b2::SourceSamples BaselineSamples(const uint8_t* data, size_t pitch) {
    b2::SourceSamples output{};
    for (unsigned cy=0;cy<16;++cy) for(unsigned cx=0;cx<16;++cx)
        for(unsigned n=0;n<2;++n) for(unsigned c=0;c<3;++c)
            output[((cy*16+cx)*2+n)*3+c] = in::Code(data,pitch,cx*16+(n?12:4),cy*16+(n?4:12),c);
    return output;
}
void Exercise() {
    std::mt19937 rng(0x527);
    std::vector<float> planar(3*in::Size*in::Size);
    size_t checks=0;
    for(unsigned iteration=0; iteration<64; ++iteration) {
        const size_t pitch=in::MinPitch+(iteration%13)*4;
        std::vector<uint8_t> raw(pitch*in::Size+3,0xcd);
        auto* data=raw.data()+(iteration%4);
        for(unsigned y=0;y<in::Size;++y) for(unsigned x=0;x<in::Size;++x) for(unsigned c=0;c<4;++c) {
            float v = iteration%2 ? float(int(rng()%4097)-1024)/2048.f : std::bit_cast<float>(uint32_t(rng()));
            std::memcpy(data+y*pitch+x*in::PixelBytes+c*sizeof(float),&v,sizeof v);
        }
        assert(in::ReadPlanar(data,pitch,planar));
        auto read=[&](unsigned x,unsigned y,unsigned c){return in::PlanarCode(planar,x,y,c);};
        const auto sig=in::ReadSignatureWith(read), old=BaselineSignature(data,pitch);
        const auto samples=b2::ReadSamplesWith(read), oldSamples=BaselineSamples(data,pitch);
        assert(std::memcmp(sig.data(),old.data(),sizeof(sig))==0);
        assert(std::memcmp(samples.data(),oldSamples.data(),sizeof(samples))==0);
        assert(std::memcmp(b2::ReadSamples(data,pitch).data(),oldSamples.data(),sizeof(samples))==0);
        checks+=sig.size()+2*samples.size();
    }
    // Actual immutable cache policy: no failed publication, one winner even
    // under concurrent misses, all callers retain a strong reference.
    for(unsigned iteration=0;iteration<128;++iteration) {
        edition::SuccessfulValueCache<std::shared_ptr<const unsigned>> cache;
        assert(!cache.Find()); assert(!cache.Publish({}));
        std::array<std::shared_ptr<const unsigned>,8> results{};
        std::array<std::thread,8> threads;
        std::atomic<unsigned> ready{0};
        for(unsigned i=0;i<threads.size();++i) threads[i]=std::thread([&,i]{
            assert(!cache.Find());
            ++ready; while(ready.load()!=threads.size()) std::this_thread::yield();
            results[i]=cache.Publish(std::make_shared<const unsigned>(i));
        });
        for(auto& thread:threads) thread.join();
        auto winner=cache.Find(); assert(winner);
        for(auto& result:results) assert(result==winner);
        assert(cache.Publish({})==winner);
        assert(cache.Publish(std::make_shared<const unsigned>(99))==winner);
    }
    std::cout << "PASS: " << checks << " bit-identical evidence values; 128 eight-thread publication races\n";
}
} // namespace
void TestPhase5ResourceReuse(){Exercise();}
#ifdef MPCVR_STANDALONE_TEST
int main(){TestPhase5ResourceReuse();}
#endif
