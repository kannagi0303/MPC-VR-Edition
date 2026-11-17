// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cwchar>
#include "NgxRuntime.h"

namespace zipdepth {
inline std::wstring LookaheadConfig()
{
    return NgxRuntime::EditionDir() + L"\\zipdepth-lookahead.ini";
}

inline unsigned LookaheadMs()
{
    static const unsigned duration = [] {
        wchar_t value[16]{};
        const DWORD length = GetEnvironmentVariableW(L"MPCVR_ZIPDEPTH_LOOKAHEAD_MS", value, _countof(value));
        if (!length) {
            const auto ms = GetPrivateProfileIntW(L"ZipDepth", L"LookaheadMs", 0, LookaheadConfig().c_str());
            return ms >= 100 && ms <= 5000 ? ms : 0u;
        }
        if (length >= _countof(value)) return 0u;
        wchar_t* end = nullptr;
        const auto ms = std::wcstoul(value, &end, 10);
        return *end == L'\0' && ms >= 100 && ms <= 5000 ? unsigned(ms) : 0u;
    }();
    return duration;
}
}
