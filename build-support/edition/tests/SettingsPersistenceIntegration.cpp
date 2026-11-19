// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the production native and Edition persistence bodies without HKCU.
#include "stdafx.h"
#include <map>
#include <iostream>
#include <stdexcept>
#include "IVideoRenderer.h"
#include "Helper.h"

struct MemoryRegistry {
    std::map<std::wstring, DWORD> values;
    std::map<std::wstring, std::wstring> strings;
    bool denyWrites = false;
    LSTATUS QueryDWORDValue(LPCWSTR key, DWORD& value) {
        const auto i = values.find(key);
        if (i == values.end()) return ERROR_FILE_NOT_FOUND;
        value = i->second; return ERROR_SUCCESS;
    }
    LSTATUS QueryStringValue(LPCWSTR key, LPWSTR value, ULONG* count) {
        const auto i = strings.find(key);
        if (i == strings.end()) return ERROR_FILE_NOT_FOUND;
        if (i->second.size() + 1 > *count) return ERROR_MORE_DATA;
        wcscpy_s(value, *count, i->second.c_str());
        *count = ULONG(i->second.size() + 1); return ERROR_SUCCESS;
    }
    LSTATUS SetDWORDValue(LPCWSTR key, DWORD value) {
        if (denyWrites) return ERROR_ACCESS_DENIED;
        values[key] = value; strings.erase(key); return ERROR_SUCCESS;
    }
    LSTATUS SetStringValue(LPCWSTR key, LPCWSTR value) {
        if (denyWrites) return ERROR_ACCESS_DENIED;
        strings[key] = value; values.erase(key); return ERROR_SUCCESS;
    }
};
#define CRegKey MemoryRegistry
#include "Edition/SettingsRegistry.cpp"
#include "Edition/RendererSettings.h"
struct SettingsPersistence {
    static void ReadSettings(CRegKey&, Settings_t&);
    static void WriteSettings(CRegKey&, const Settings_t&);
};
#include "SettingsPersistence.generated.inc"
#undef CRegKey

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() try {
    MemoryRegistry input;
    input.values = {{L"UseD3D11", 0}, {L"TextureFormat", 0xffffffff},
        {L"ChromaUpsampling", 0xffffffff}, {L"Upscaling", 0xffffffff},
        {L"VPSuperResolution", SUPERRES_Always}, {L"MfgMultiplier", 3},
        {L"WaitForExtendedStartup", 1}, {L"SharpenLevel", 0xffffffff},
        {L"ZipDepthMode", ZIPDEPTH_Both}, {L"SvdLutStrength", 67},
        {L"MfgLowMotion", 1}, {L"SdrPeakWindowMs", SDR_PEAK_WINDOW_MIN},
        {L"SdrPeakFloorNits", SDR_PEAK_FLOOR_MIN}, {L"SdrPeakSceneCuts", 0}};
    input.strings[L"DlssSRDllPath"] = L"test-only.dll";
    Settings_t loaded;
    const auto peakWindow = loaded.iSdrPeakWindowMs;
    const auto peakFloor = loaded.iSdrPeakFloorNits;
    const auto peakCuts = loaded.bSdrPeakSceneCuts;
    SettingsPersistence::ReadSettings(input, loaded);
    Require(!loaded.bUseD3D11 && loaded.iTexFormat == TEXFMT_AUTOINT, "Native settings validation");
    Require(loaded.iChromaScaling == CHROMA_CatmullRom && loaded.iUpscaling == UPSCALE_Jinc2,
        "Edition fallback policy must follow native validation");
    Require(loaded.iVPSuperRes == SUPERRES_1440p && loaded.iMfgMultiplier == 7,
        "Legacy VSR/MFG migration");
    Require(loaded.bWaitForExtendedStartup && loaded.bUseZipDepthSr && loaded.bUseZipDepthMfg &&
        loaded.GetSvdLutStrength() == 67 && !loaded.bMfgLowMotion, "Edition settings and scope");
    Require(std::wstring(loaded.szDlssSRDllPath) == L"test-only.dll", "Edition string settings");
    Require(loaded.iSdrPeakWindowMs == peakWindow && loaded.iSdrPeakFloorNits == peakFloor &&
        loaded.bSdrPeakSceneCuts == peakCuts, "Preset read must exclude registry-only tuning");
    ReadStartupTuning(input, loaded);
    Require(loaded.iSdrPeakWindowMs == SDR_PEAK_WINDOW_MIN && loaded.iSdrPeakFloorNits == SDR_PEAK_FLOOR_MIN &&
        !loaded.bSdrPeakSceneCuts, "Startup must retain native registry-only tuning");

    MemoryRegistry saved;
    SettingsPersistence::WriteSettings(saved, loaded);
    Require(edition::VerifySavedSettings(saved, loaded) == S_OK, "Save verification");
    Require(saved.values.at(L"UseD3D11") == 0 && saved.values.at(L"MfgMultiplier") == 7 &&
        saved.values.at(L"SvdLutStrength") == 67, "Native and Edition write composition");
    Require(!saved.values.count(L"SdrPeakWindowMs") && !saved.values.count(L"SdrPeakFloorNits") &&
        !saved.values.count(L"SdrPeakSceneCuts"), "Registry-only tuning must never be written");
    Settings_t restored;
    SettingsPersistence::ReadSettings(saved, restored);
    MemoryRegistry savedAgain;
    SettingsPersistence::WriteSettings(savedAgain, restored);
    Require(saved.values == savedAgain.values && saved.strings == savedAgain.strings, "Preset roundtrip");

    auto corrupt = saved;
    corrupt.values[L"SvdLutStrength"] = 0;
    Require(edition::VerifySavedSettings(corrupt, loaded) == E_FAIL, "Mismatched save rejection");
    saved.values.erase(L"MfgMultiplier");
    Require(edition::VerifySavedSettings(saved, loaded) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        "Missing save key must retain HRESULT");
    MemoryRegistry denied;
    denied.denyWrites = true;
    SettingsPersistence::WriteSettings(denied, loaded);
    Require(FAILED(edition::VerifySavedSettings(denied, loaded)), "Denied writes must not report success");
    for (DWORD raw : {0u, 1u, 4u, 5u, 6u, 7u, 0xffffffffu}) {
        MemoryRegistry key;
        key.values[L"VPSuperResolution"] = raw;
        Settings_t sets;
        SettingsPersistence::ReadSettings(key, sets);
        const int expected = raw == SUPERRES_Always ? SUPERRES_1440p
            : raw < SUPERRES_DLSS_SR ? int(raw) : SUPERRES_Disable;
        Require(sets.iVPSuperRes == expected, "VSR boundary value");
        Require(sets.bDlssSR == (raw == SUPERRES_DLSS_SR), "Legacy combined DLSS mode migration");
    }
    std::cout << "PASS: actual native/Edition settings, preset roundtrip, registry-only tuning, save failures and VSR boundaries; no real registry access.\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
}
