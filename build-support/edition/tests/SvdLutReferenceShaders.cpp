// SPDX-License-Identifier: GPL-3.0-or-later
// Run in a VS Developer Command Prompt, Windows SDK installed:
// cl /nologo /EHsc /std:c++20 SvdLutReferenceShaders.cpp d3dcompiler.lib
// SvdLutReferenceShaders.exe
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <string>
#include <iostream>
#include "../../../Source/DLSS/SvdLutShader.h"
#include "../../../Source/DLSS/PerceptualColorShader.h"
#include "../../../Source/Edition/AdaptiveToneShader.h"
using Microsoft::WRL::ComPtr;

bool Compile(const char* name, const char* source, const D3D_SHADER_MACRO* defines,
             unsigned bytes, bool graded)
{
    std::string text(source);
    if (graded) text = std::string(SvdLutAdaptiveLumaShader) + (source == PerceptualColorShader ? std::string(AdaptiveToneShader) : std::string()) + text + SvdLutOutputShader;
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(text.data(), text.size(), name, defines, nullptr,
        "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "FAIL " << name << ": " << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed") << '\n';
        return false;
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
        __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(reflection.GetAddressOf())))) return false;
    D3D11_SHADER_BUFFER_DESC cb{};
    auto* buffer = reflection->GetConstantBufferByName(graded ? "Parameters" : "InputRect");
    if (!buffer || FAILED(buffer->GetDesc(&cb)) || cb.Size != bytes) {
        std::cerr << "FAIL " << name << " constant-buffer size " << cb.Size << " expected " << bytes << '\n';
        return false;
    }
    if (graded) {
        D3D11_SHADER_INPUT_BIND_DESC d{};
        if (FAILED(reflection->GetResourceBindingDescByName("OutputDither", &d)) || d.BindPoint != 4) {
            std::cerr << "FAIL " << name << " dither SRV contract\n";
            return false;
        }
    }
    std::cout << "PASS " << name << '\n';
    return true;
}
int main()
{
    const D3D_SHADER_MACRO m1[] = {{"SVDLUT_M1_SKIN_GUARD","1"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO r1[] = {{"SVDLUT_R1_CONTINUOUS","1"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO both[] = {{"SVDLUT_R1_CONTINUOUS","1"},{"SVDLUT_M1_SKIN_GUARD","1"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO a[] = {{"P1_NORMAL_A","1"},{nullptr,nullptr}};
    const D3D_SHADER_MACRO b[] = {{"P1_RAW_B","1"},{nullptr,nullptr}};
    bool ok = true;
    ok &= Compile("thumbnail", SvdLutThumbnailShader, nullptr, 16, false);
    ok &= Compile("AT thumbnail", AdaptiveToneThumbnail, nullptr, 16, false);
    ok &= Compile("A", SvdLutShader, nullptr, 48, true);
    ok &= Compile("RAW/B1", SvdLutRawShader, nullptr, 64, true);
    ok &= Compile("B2.1", SvdLutB2LuminanceShader, nullptr, 64, true);
    ok &= Compile("B2.2", SvdLutB2MemoryShader, nullptr, 64, true);
    ok &= Compile("M1", SvdLutB2MemoryShader, m1, 64, true);
    ok &= Compile("R1", SvdLutB2MemoryShader, r1, 64, true);
    ok &= Compile("R1+M1", SvdLutB2MemoryShader, both, 64, true);
    ok &= Compile("P1 standalone", PerceptualColorShader, nullptr, 96, true);
    ok &= Compile("P1+A", PerceptualColorShader, a, 96, true);
    ok &= Compile("P1+B", PerceptualColorShader, b, 96, true);
    const std::string at=std::string(AdaptiveToneShader)+AdaptiveToneStandalone;
    ComPtr<ID3DBlob> code,errors;
    HRESULT hr=D3DCompile(at.data(),at.size(),"AT standalone",nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,code.GetAddressOf(),errors.GetAddressOf());
    if (FAILED(hr)) { std::cerr<<"FAIL AT standalone\n"; ok=false; }
    else { ComPtr<ID3D11ShaderReflection> reflection; D3D11_SHADER_BUFFER_DESC desc{}; D3D11_SHADER_INPUT_BIND_DESC binding{};
        ok &= SUCCEEDED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),__uuidof(ID3D11ShaderReflection),reinterpret_cast<void**>(reflection.GetAddressOf()))) &&
            SUCCEEDED(reflection->GetConstantBufferByName("Parameters")->GetDesc(&desc)) && desc.Size==48 &&
            SUCCEEDED(reflection->GetConstantBufferByName("AdaptiveToneCurve")->GetDesc(&desc)) && desc.Size==1040 &&
            SUCCEEDED(reflection->GetResourceBindingDescByName("AdaptiveToneCurve",&binding)) && binding.BindPoint==1;
    }
    return ok ? 0 : 1;
}
