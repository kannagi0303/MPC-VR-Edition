// SPDX-License-Identifier: GPL-3.0-or-later
// Portable production-reader regression: no D3D, PyTorch or Windows required.
#include "../../../Source/Edition/SvdLutInput.h"
#include "../../../Source/Edition/SvdLutB2Refined.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

int main()
{
    namespace in = svdlut::input;
    constexpr size_t pitch = in::MinPitch + 36;
    // Deliberately unaligned mapped pointer and padded row pitch.
    std::vector<uint8_t> bytes(pitch * in::Size + 1, 0xCD);
    auto* data = bytes.data() + 1;
    auto put = [&](unsigned x, unsigned y, unsigned c, float v) {
        std::memcpy(data + y * pitch + x * in::PixelBytes + c * sizeof(float), &v, sizeof(v));
    };
    auto expected = [](unsigned x, unsigned y, unsigned c) {
        return c == 0 ? (x + .25f) / 256.0f : c == 1 ? (y + .75f) / 256.0f : .1234567f;
    };
    for (unsigned y = 0; y < in::Size; ++y)
        for (unsigned x = 0; x < in::Size; ++x) {
            for (unsigned c = 0; c < 3; ++c) put(x,y,c,expected(x,y,c));
            put(x,y,3, .987654f);
        }
    std::vector<float> image(3 * in::Size * in::Size);
    assert(in::ReadPlanar(data, pitch, image));
    for (unsigned y = 0; y < in::Size; ++y)
        for (unsigned x = 0; x < in::Size; ++x)
            for (unsigned c = 0; c < 3; ++c)
                assert(image[c * in::Size * in::Size + y * in::Size + x] == expected(x,y,c));
    const auto samples = svdlut::b2::refined::ReadSamples(data,pitch);
    for (unsigned cy=0;cy<16;++cy) for(unsigned cx=0;cx<16;++cx)
        for(unsigned n=0;n<2;++n) for(unsigned c=0;c<3;++c)
            assert(samples[((cy*16+cx)*2+n)*3+c] == expected(cx*16+(n?12:4),cy*16+(n?4:12),c));
    assert(!in::ReadPlanar(data,in::MinPitch-1,image));
    assert(!in::ReadPlanar(nullptr,pitch,image));
    put(0,0,0,-.25f); put(0,0,1,1.5f); put(0,0,2,std::numeric_limits<float>::quiet_NaN());
    assert(in::ReadPlanar(data,pitch,image));
    assert(image[0]==0 && image[in::Size*in::Size]==1 && image[2*in::Size*in::Size]==0);
    std::cout << "PASS: 196608 planar values + 1536 B2 sample values, padded/unaligned rows, invalid inputs\n";
}
