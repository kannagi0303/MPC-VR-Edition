// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone Windows SDK test: compile as C++20 and link d3dcompiler.lib.
// No GPU, DirectShow registration or model files required.
#include "../../../Source/Edition/FixedShaderBytecode.h"
#include "../../../Source/Mfg/MotionShaders.h"
#include "../../../Source/DLSS/SrMotionRefine.h"
#include "../../../Source/Depth/DepthGpuShaders.h"
#undef NDEBUG
#include <cassert>
#include <cstring>
#include <iostream>
#pragma comment(lib,"d3dcompiler.lib")

template<auto& Source, edition::FixedShaderString Entry, edition::FixedShaderString Profile>
void Verify() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3DBlob> baseline, first, second;
    assert(SUCCEEDED(D3DCompile(Source,sizeof(Source)-1,nullptr,nullptr,nullptr,
        Entry.text,Profile.text,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&baseline,nullptr)));
    assert(SUCCEEDED((edition::CompileFixedShader<Source,Entry,Profile>(&first))));
    assert(SUCCEEDED((edition::CompileFixedShader<Source,Entry,Profile>(&second))));
    assert(first.Get()==second.Get());
    assert(first->GetBufferSize()==baseline->GetBufferSize());
    assert(std::memcmp(first->GetBufferPointer(),baseline->GetBufferPointer(),first->GetBufferSize())==0);
}
int main() {
    Verify<mfg::MotionShaders,"VS","vs_5_0">();
    Verify<mfg::MotionShaders,"Luma","ps_5_0">();
    Verify<mfg::MotionShaders,"Copy","ps_5_0">();
    Verify<mfg::MotionShaders,"MfgVectors","ps_5_0">();
    Verify<mfg::MotionShaders,"Vectors","ps_5_0">();
    Verify<mfg::MotionShaders,"PictureRows","ps_5_0">();
    Verify<mfg::MotionShaders,"ActivePicture","ps_5_0">();
    Verify<sr_motion::RefineShaders,"VectorsValidatedSR","ps_5_0">();
    Verify<sr_motion::RefineShaders,"VectorsValidatedCostSR","ps_5_0">();
    Verify<mfg::MotionShaders,"VectorsValidated","ps_5_0">();
    Verify<mfg::MotionShaders,"VectorsValidatedCost","ps_5_0">();
    Verify<DepthGpuShaders,"Pack","cs_5_0">();
    Verify<DepthGpuShaders,"RawRangeTiles","cs_5_0">();
    Verify<DepthGpuShaders,"RawRange","cs_5_0">();
    Verify<DepthGpuShaders,"RawNormalize","cs_5_0">();
    std::cout<<"PASS: 15 fixed shader keys; cached blob identity and compiler bytecode parity\n";
    return 0;
}
