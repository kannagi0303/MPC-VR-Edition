// SPDX-License-Identifier: GPL-3.0-or-later
// Loads a filter directly and exercises its real hidden property page against an
// in-memory renderer. No filter registration, player, GPU session, or registry writes.
#include <windows.h>
#include <VersionHelpers.h>
#include <ocidl.h>
#include <commctrl.h>
#include <atlbase.h>
#include <d3d9.h>
#include <string>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include "IVideoRenderer.h"
#include "SettingsLayoutContract.h"
#include "FilterInterfaces.h"
#include "resource.h"

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Check(HRESULT value, const char* message) { Require(SUCCEEDED(value), message); }

class MemoryRenderer final : public IVideoRenderer, public IExFilterConfig {
    ULONG references = 1;
public:
    Settings_t settings;
    unsigned saves = 0;
    HRESULT saveResult = S_OK;
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown || id == __uuidof(IVideoRenderer)) *out = static_cast<IVideoRenderer*>(this);
        else if (id == __uuidof(IExFilterConfig)) *out = static_cast<IExFilterConfig*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
    STDMETHODIMP_(ULONG) Release() override { return --references; }
    STDMETHODIMP GetVideoProcessorInfo(std::wstring& text) override { text = L"Edition test"; return S_OK; }
    STDMETHODIMP_(bool) GetActive() override { return false; }
    STDMETHODIMP_(void) GetSettings(Settings_t& value) override { value = settings; }
    STDMETHODIMP_(void) SetSettings(const Settings_t& value) override { settings = value; }
    STDMETHODIMP SaveSettings() override { ++saves; return saveResult; }
    STDMETHODIMP_(unsigned) GetVideoProcessorUse() override { return 0; }
    STDMETHODIMP SavePreset(int, const Settings_t&, LPCWSTR, int) override { return E_NOTIMPL; }
    STDMETHODIMP LoadPreset(int, Settings_t&) override { return S_FALSE; }
    STDMETHODIMP GetPresetInfo(int, LPWSTR, int, int*) override { return S_FALSE; }
    STDMETHODIMP SetPresetInfo(int, LPCWSTR, int) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_GetBool(LPCSTR, bool*) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_GetInt(LPCSTR, int*) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_GetInt64(LPCSTR, __int64*) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_GetDouble(LPCSTR, double*) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_GetString(LPCSTR field, LPWSTR* value, unsigned*) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (std::strcmp(field, "projectionProfileName")) return E_NOTIMPL;
        constexpr wchar_t name[] = L"Integration test";
        *value = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
        if (!*value) return E_OUTOFMEMORY;
        std::memcpy(*value, name, sizeof(name)); return S_OK;
    }
    STDMETHODIMP Flt_GetBin(LPCSTR, LPVOID*, unsigned*) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_SetBool(LPCSTR, bool) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_SetInt(LPCSTR field, int value) override {
        if (std::strcmp(field, "svdLutStrengthPreview")) return E_NOTIMPL;
        settings.SetSvdLutStrength(value); return S_OK;
    }
    STDMETHODIMP Flt_SetInt64(LPCSTR, __int64) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_SetDouble(LPCSTR, double) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_SetString(LPCSTR, LPWSTR, int) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_SetBin(LPCSTR, LPVOID, int) override { return E_NOTIMPL; }
};

class PageSite final : public IPropertyPageSite {
    ULONG references = 1;
public:
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IPropertyPageSite) return E_NOINTERFACE;
        *out = static_cast<IPropertyPageSite*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
    STDMETHODIMP_(ULONG) Release() override { return --references; }
    STDMETHODIMP OnStatusChange(DWORD) override { return S_OK; }
    STDMETHODIMP GetLocaleID(LCID* value) override { if (!value) return E_POINTER; *value = 1033; return S_OK; }
    STDMETHODIMP GetPageContainer(IUnknown**) override { return E_NOTIMPL; }
    STDMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
};

struct Page {
    MemoryRenderer renderer;
    PageSite site;
    CComPtr<IPropertyPage> object;
    HWND host = nullptr, window = nullptr;
    bool active = false, connected = false;
    explicit Page(HMODULE module) {
        renderer.settings.bUseD3D11 = true;
        renderer.settings.bJvrFlatView = true;
        renderer.settings.iJvrVertical = 17;
        renderer.settings.iJvrEye = 1;
        renderer.settings.SetSvdLutStrength(25);
        using GetFactory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        const auto factoryEntry = reinterpret_cast<GetFactory>(GetProcAddress(module, "DllGetClassObject"));
        Require(factoryEntry != nullptr, "Missing class factory export");
        const CLSID pageId = {0xDA46D181,0x07D6,0x441D,{0xB3,0x14,0x01,0x9A,0xEB,0x10,0x14,0x8A}};
        CComPtr<IClassFactory> factory;
        Check(factoryEntry(pageId, IID_PPV_ARGS(&factory)), "Cannot get real property page factory");
        Check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&object)), "Cannot create property page");
        Check(object->SetPageSite(&site), "Cannot attach page site");
        IUnknown* unknown = static_cast<IVideoRenderer*>(&renderer);
        Check(object->SetObjects(1, &unknown), "Cannot attach memory renderer");
        connected = true;
        host = CreateWindowExW(0, L"STATIC", L"Edition integration", WS_OVERLAPPED, 0, 0, 800, 900,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(host != nullptr, "Cannot create hidden page host");
        Activate();
    }
    ~Page() {
        if (active) object->Deactivate();
        if (connected) object->SetObjects(0, nullptr);
        if (object) object->SetPageSite(nullptr);
        object.Release();
        if (host) DestroyWindow(host);
    }
    void Activate() {
        const RECT bounds{0,0,800,900};
        Check(object->Activate(host, &bounds, FALSE), "Cannot activate real page");
        active = true;
        window = GetWindow(host, GW_CHILD);
        Require(window && GetDlgItem(window, IDC_SVDLUT_STRENGTH), "Resource include lost Edition controls");
    }
    void Deactivate() { Check(object->Deactivate(), "Cannot deactivate page"); active = false; }
    void Cancel() { Deactivate(); Check(object->SetObjects(0, nullptr), "Cannot disconnect page"); connected = false; }
    void Slider(int id, int value) {
        SendDlgItemMessageW(window, id, TBM_SETPOS, TRUE, value);
        SendMessageW(window, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(GetDlgItem(window, id)));
    }
    void Click(int id, bool checked) {
        CheckDlgButton(window, id, checked ? BST_CHECKED : BST_UNCHECKED);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, id)));
    }
    void Select(int id, LONG_PTR data) {
        const auto control = GetDlgItem(window, id);
        const auto count = SendMessageW(control, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            if (SendMessageW(control, CB_GETITEMDATA, i, 0) == data) {
                SendMessageW(control, CB_SETCURSEL, i, 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(control));
                return;
            }
        }
        throw std::runtime_error("Missing Edition combo selection");
    }
};

void Run(HMODULE module, bool profiles) {
    {
        Page page(module);
        page.Slider(IDC_SVDLUT_STRENGTH, 55);
        Require(page.renderer.settings.GetSvdLutStrength() == 55 && page.renderer.saves == 0, "Strength preview saved or failed");
        if (profiles) {
            page.Slider(IDC_JVR_VERTICAL, 15);
            page.Select(IDC_JVR_EYE, 0);
            Require(page.renderer.settings.iJvrVertical == 15 && page.renderer.settings.iJvrEye == 0, "JVR preview failed");
        }
        page.Click(IDC_MFG_MULTIPLIER, true);
        page.Select(IDC_DLSS_SR_SOURCE_LIMIT, DLSSSR_SOURCE_1080p);
        page.Select(IDC_ZIPDEPTH_MODE, ZIPDEPTH_DlssSrOnly);
        page.Select(IDC_MFG_PROCESSING_MODE, 1);
        page.Select(IDC_EXTENDED_STARTUP, 1);
        Check(page.object->Apply(), "Cannot apply Edition controls");
        const auto& settings = page.renderer.settings;
        Require(page.renderer.saves == 1 && settings.iMfgMultiplier == 7 && settings.bDlssSR &&
            settings.iDlssSRSourceLimit == DLSSSR_SOURCE_1080p && settings.bUseZipDepthSr && !settings.bUseZipDepthMfg &&
            settings.iMfgProcessingMode == 1 && settings.bWaitForExtendedStartup, "Apply lost Edition fields");
        page.Slider(IDC_SVDLUT_STRENGTH, 80);
        if (profiles) page.Slider(IDC_JVR_VERTICAL, -20);
        page.Cancel();
        Require(settings.GetSvdLutStrength() == 55 && settings.iJvrVertical == (profiles ? 15 : 17), "Cancel did not restore applied preview");
    }
    {
        Page page(module);
        page.Slider(IDC_SVDLUT_STRENGTH, 65);
        page.Deactivate(); page.Activate();
        page.Cancel();
        Require(page.renderer.settings.GetSvdLutStrength() == 25, "Reactivation lost strength rollback checkpoint");
    }
    {
        Page page(module);
        page.renderer.saveResult = E_FAIL;
        page.Slider(IDC_SVDLUT_STRENGTH, 75);
        Require(page.object->Apply() == E_FAIL, "Apply hid save failure");
        page.Cancel();
        Require(page.renderer.settings.GetSvdLutStrength() == 25, "Failed save advanced rollback checkpoint");
    }
    {
        Page page(module);
        page.Slider(IDC_SVDLUT_STRENGTH, 45);
        SendDlgItemMessageW(page.window, IDC_ZIPDEPTH_MODE, CB_SETCURSEL, -1, 0);
        Require(page.object->Apply() == E_INVALIDARG && page.renderer.saves == 0, "Invalid selection was committed");
        page.Cancel();
    }
    if (!profiles) {
        Page page(module);
        page.Click(IDC_BUTTON1, false);
        Check(page.object->Apply(), "Cannot apply defaults");
        Require(page.renderer.settings.bJvrFlatView && page.renderer.settings.iJvrVertical == 17 &&
            page.renderer.settings.iJvrEye == 1, "Defaults changed hidden JVR settings");
    }
    {
        Page page(module);
        page.Click(IDC_CHECK1, false);
        Require(!IsWindowEnabled(GetDlgItem(page.window, IDC_MFG_MULTIPLIER)) &&
            !IsWindowEnabled(GetDlgItem(page.window, IDC_DLSS_SR_SOURCE_LIMIT)), "D3D11 gate lost");
    }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 3, "Usage: EditionPageIntegration.exe <filter.ax> <profiles:0|1>");
        Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization failed");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "Common controls initialization failed");
        HMODULE module = LoadLibraryW(argv[1]);
        Require(module != nullptr, "Cannot load filter directly");
        Run(module, argv[2][0] == L'1');
        FreeLibrary(module);
        CoUninitialize();
        std::cout << "PASS: real Edition page preview/apply/cancel, save failure, reactivation, selection validation, D3D11 gate.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
