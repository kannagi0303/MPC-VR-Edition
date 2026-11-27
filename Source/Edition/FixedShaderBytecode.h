// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SuccessfulValueCache.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstddef>

namespace edition {
// Compile-time identity: exact source array, entry, profile. Only fixed,
// macro-free, include-free, optimization-level-3 shaders use this cache.
// Bytecode is CPU-only, never holds a D3D device, context, PSO or GPU fence.
template<std::size_t N> struct FixedShaderString {
    char text[N]{};
    constexpr FixedShaderString(const char (&value)[N]) {
        for (std::size_t i = 0; i < N; ++i) text[i] = value[i];
    }
};

template<auto& Source, FixedShaderString Entry, FixedShaderString Profile>
HRESULT CompileFixedShader(ID3DBlob** output, ID3DBlob** errors = nullptr) {
    using Microsoft::WRL::ComPtr;
    static SuccessfulValueCache<ComPtr<ID3DBlob>> cache;
    if (auto code = cache.Find()) return code.CopyTo(output);
    ComPtr<ID3DBlob> code;
    const auto hr = D3DCompile(Source, sizeof(Source) - 1, nullptr, nullptr, nullptr,
        Entry.text, Profile.text, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors);
    if (FAILED(hr)) return hr; // retry next time; preserve diagnostics
    if (!code) return E_FAIL;
    return cache.Publish(code).CopyTo(output);
}
}
