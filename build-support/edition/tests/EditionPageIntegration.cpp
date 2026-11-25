// SPDX-License-Identifier: GPL-3.0-or-later
// Loads a filter directly against an in-memory renderer. The default suite uses
// a hidden property page; --visual-tabs tests real visible drawing separately.
// No filter registration, player, GPU session, or registry writes.
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
    unsigned saves = 0, settingsWrites = 0, previewWrites = 0;
    int videoEnhancement=0, adaptiveTone=0, memoryColor=0, skinTone=0;
    bool enhancementEnabled=true;
    int* Strength(LPCSTR field) {
        if(!std::strcmp(field,"colorEnhancementStrength")) return &videoEnhancement;
        if(!std::strcmp(field,"adaptiveToneStrength")) return &adaptiveTone;
        if(!std::strcmp(field,"memoryColorStrength")) return &memoryColor;
        if(!std::strcmp(field,"skinToneStrength")) return &skinTone;
        return nullptr;
    }
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
    STDMETHODIMP_(void) SetSettings(const Settings_t& value) override { ++settingsWrites; settings = value; }
    STDMETHODIMP SaveSettings() override { ++saves; return saveResult; }
    STDMETHODIMP_(unsigned) GetVideoProcessorUse() override { return 0; }
    STDMETHODIMP SavePreset(int, const Settings_t&, LPCWSTR, int) override { return E_NOTIMPL; }
    STDMETHODIMP LoadPreset(int, Settings_t&) override { return S_FALSE; }
    STDMETHODIMP GetPresetInfo(int, LPWSTR, int, int*) override { return S_FALSE; }
    STDMETHODIMP SetPresetInfo(int, LPCWSTR, int) override { return E_NOTIMPL; }
    STDMETHODIMP Flt_GetBool(LPCSTR field, bool* value) override {
        if (!std::strcmp(field,"enhancementEnabled")) { *value=enhancementEnabled; return S_OK; }
        if (!std::strcmp(field,"adaptiveTone")) { *value=adaptiveTone; return S_OK; }
        if (std::strcmp(field, "videoEnhancement")) return E_NOTIMPL;
        *value = videoEnhancement; return S_OK;
    }
    STDMETHODIMP Flt_GetInt(LPCSTR field, int* value) override { if(auto* strength=Strength(field)){*value=*strength;return S_OK;}return E_NOTIMPL; }
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
    STDMETHODIMP Flt_SetBool(LPCSTR field, bool value) override {
        if (!std::strcmp(field,"enhancementEnabled")) { ++previewWrites; enhancementEnabled=value; return S_OK; }
        if (!std::strcmp(field,"adaptiveTone")) { adaptiveTone=value; return S_OK; }
        if (std::strcmp(field, "videoEnhancement")) return E_NOTIMPL;
        videoEnhancement = value; return S_OK;
    }
    STDMETHODIMP Flt_SetInt(LPCSTR field, int value) override { if(auto* strength=Strength(field)){++previewWrites;*strength=value;return S_OK;}return E_NOTIMPL; }
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
        renderer.settings.SetSvdLutStrength(25); // retired ABI slot; must not control VE
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
        Require(window && Control(IDC_VIDEO_ENHANCEMENT), "Resource include lost Video Enhancement");
        for (int id : {IDC_SVDLUT_STRENGTH, IDC_SVDLUT_MODE, IDC_SVDLUT_B_STRENGTH,
                       IDC_SVDLUT_B2_LUMINANCE, IDC_SVDLUT_B2_MEMORY, IDC_SVDLUT_R1_GAMUT,
                       IDC_PREFERRED_COLORS, IDC_SKIN_TONES})
            Require(!Control(id), "Retired experiment control still exposed");
    }
    void Deactivate() { Check(object->Deactivate(), "Cannot deactivate page"); active = false; }
    void Cancel() { Deactivate(); Check(object->SetObjects(0, nullptr), "Cannot disconnect page"); connected = false; }
    void Tab(int index) {
        auto tab=GetDlgItem(window,IDC_SETTINGS_TAB);
        SendMessageW(tab,TCM_SETCURSEL,index,0);
        NMHDR header{tab,IDC_SETTINGS_TAB,TCN_SELCHANGE};
        SendMessageW(window,WM_NOTIFY,IDC_SETTINGS_TAB,reinterpret_cast<LPARAM>(&header));
    }
    HWND Control(int id) const {
        if (auto direct = GetDlgItem(window, id)) return direct;
        for (int paneId : {IDD_EDITION_RENDERER_PANE, IDD_EDITION_EXTENDED_PANE})
            if (auto pane = GetDlgItem(window, paneId))
                if (auto control = GetDlgItem(pane, id)) return control;
        return nullptr;
    }
    bool Visible(int id) const {
        // The integration host is intentionally hidden. Check the control and
        // every ancestor up to the property page, not the host's visibility.
        HWND control=Control(id);
        if (!control) return false;
        for (HWND h=control; h && h!=window; h=GetParent(h))
            if (!(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)) return false;
        return true;
    }
    void Notify(int id, UINT msg, WPARAM wp) {
        auto control=Control(id);
        Require(control!=nullptr,"Missing page control");
        SendMessageW(GetParent(control),msg,wp,reinterpret_cast<LPARAM>(control));
    }
    void Slider(int id, int value) {
        SendMessageW(Control(id),TBM_SETPOS,TRUE,value);
        Notify(id,WM_HSCROLL,TB_THUMBPOSITION);
    }
    void Edit(int id, const wchar_t* text, bool finish=true) {
        SetWindowTextW(Control(id),text);
        if(finish) Notify(id,WM_COMMAND,MAKEWPARAM(id,EN_KILLFOCUS));
    }
    int Numeric(int id) { return static_cast<int>(GetDlgItemInt(GetParent(Control(id)),id,nullptr,TRUE)); }
    void Click(int id, bool checked) {
        SendMessageW(Control(id),BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
        Notify(id,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED));
    }
    void Select(int id, LONG_PTR data) {
        const auto control = Control(id);
        const auto count = SendMessageW(control, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i) {
            if (SendMessageW(control, CB_GETITEMDATA, i, 0) == data) {
                SendMessageW(control, CB_SETCURSEL, i, 0);
                Notify(id,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE));
                return;
            }
        }
        throw std::runtime_error("Missing Edition combo selection");
    }
};

// An actual visible HWND/DC is essential here: a memory DC has no child-window
// clipping and would report success even when a transparent group box prevents
// the pane from erasing its real client area.
void PumpPaintMessages() {
    for (int pass=0; pass<3; ++pass) {
        MSG msg{};
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        GdiFlush();
        Sleep(15);
    }
}

void CheckVisibleGroupBackground(HWND pane, HWND host, unsigned expectedGroups) {
    Require(IsWindowVisible(pane)!=FALSE,"Selected pane is not visible on screen");
    // DCX_USESTYLE reproduces the real pane's window-style clipping, unlike
    // the FIX04 memory-DC erase check. The outer host may still clip its panes.
    HDC paneDC=GetDCEx(pane,nullptr,DCX_CACHE|DCX_USESTYLE);
    HDC screenDC=GetDC(nullptr);
    if (!paneDC || !screenDC) {
        if(paneDC) ReleaseDC(pane,paneDC);
        if(screenDC) ReleaseDC(nullptr,screenDC);
        Require(false,"Cannot acquire visible window/screen DC");
    }
    unsigned groups=0, points=0;
    const char* failure=nullptr;
    const COLORREF background=GetSysColor(COLOR_3DFACE);
    for(HWND group=GetWindow(pane,GW_CHILD); group && !failure; group=GetWindow(group,GW_HWNDNEXT)) {
        const auto style=GetWindowLongPtrW(group,GWL_STYLE);
        wchar_t cls[32]{};
        GetClassNameW(group,cls,32);
        if(!(style&WS_VISIBLE) || (style&BS_TYPEMASK)!=BS_GROUPBOX || lstrcmpiW(cls,L"Button")!=0) continue;
        ++groups;
        RECT bounds{};
        GetWindowRect(group,&bounds);
        unsigned sampled=0;
        // Sample blank group interiors, outside captions, borders and every
        // other visible sibling control. Choose points from actual DPI-scaled HWNDs.
        for(LONG y=bounds.top+24; y<bounds.bottom-10 && sampled<3 && !failure; y+=11) {
            for(LONG x=bounds.left+10; x<bounds.right-10 && sampled<3; x+=17) {
                POINT at{x,y}; bool occupied=false;
                for(HWND child=GetWindow(pane,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT)) {
                    if(child==group || !(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)) continue;
                    RECT rect{}; GetWindowRect(child,&rect); InflateRect(&rect,3,3);
                    if(PtInRect(&rect,at)) { occupied=true; break; }
                }
                if(occupied) continue;
                const HWND hit=WindowFromPoint(at);
                if(hit!=host && !IsChild(host,hit)) { failure="Visible test window is obscured"; break; }
                POINT local=at; ScreenToClient(pane,&local);
                if(!PtVisible(paneDC,local.x,local.y)) {
                    failure="Group interior is clipped out of the pane's real DC"; break;
                }
                if(GetPixel(screenDC,at.x,at.y)!=background) {
                    failure="Group interior contains stale pixels or wrong background"; break;
                }
                ++sampled; ++points;
            }
        }
        if(!sampled && !failure) failure="No unobstructed group interior pixel could be tested";
    }
    ReleaseDC(nullptr,screenDC);
    ReleaseDC(pane,paneDC);
    Require(!failure,failure?failure:"Visible paint failed");
    Require(groups==expectedGroups && points>=groups,"Unexpected number of visible group backgrounds tested");
}

void RunVisibleTabs(HMODULE module, bool profiles) {
    Page page(module);
    PROPPAGEINFO info{}; info.cb=sizeof(info);
    Check(page.object->GetPageInfo(&info),"Cannot get page dimensions");
    const RECT bounds{0,0,info.size.cx,info.size.cy};
    CoTaskMemFree(info.pszTitle); CoTaskMemFree(info.pszDocString); CoTaskMemFree(info.pszHelpFile);
    Check(page.object->Move(&bounds),"Cannot size visible property page");
    RECT work{}; Require(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)!=FALSE,"Cannot obtain display work area");
    // The test host must not erase over the opaque property page itself.
    SetWindowLongPtrW(page.host,GWL_STYLE,GetWindowLongPtrW(page.host,GWL_STYLE)|WS_CLIPCHILDREN);
    RECT hostBounds=bounds;
    Require(AdjustWindowRectEx(&hostBounds,static_cast<DWORD>(GetWindowLongPtrW(page.host,GWL_STYLE)),FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(page.host,GWL_EXSTYLE)))!=FALSE,"Cannot size visible host frame");
    const int width=hostBounds.right-hostBounds.left, height=hostBounds.bottom-hostBounds.top;
    Require(width<=work.right-work.left && height<=work.bottom-work.top,
        "Display work area too small for unobscured visible test");
    Require(SetWindowPos(page.host,HWND_TOPMOST,work.left,work.top,width,height,
        SWP_SHOWWINDOW)!=FALSE,"Cannot show visible test host");
    // Initial exposure only. Later transitions must paint via the production
    // tab path, without the test issuing a compensating RedrawWindow call.
    RedrawWindow(page.host,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    PumpPaintMessages();
    HWND tab=page.Control(IDC_SETTINGS_TAB);
    HWND native=page.Control(IDD_EDITION_RENDERER_PANE), extended=page.Control(IDD_EDITION_EXTENDED_PANE);
    Require(native && extended && tab,"Missing actual tab panes");
    Require(!(GetWindowLongPtrW(native,GWL_STYLE)&WS_CLIPCHILDREN) &&
        !(GetWindowLongPtrW(extended,GWL_STYLE)&WS_CLIPCHILDREN),"Pane clips transparent group interiors");
    CheckVisibleGroupBackground(native,page.host,3);
    for(int i=0;i<40;++i) {
        const int index=(i%2==0)?1:0;
        RECT item{}; Require(TabCtrl_GetItemRect(tab,index,&item)!=FALSE,"Cannot locate tab");
        const LPARAM point=MAKELPARAM((item.left+item.right)/2,(item.top+item.bottom)/2);
        // Drive the actual tab control, not a fabricated TCN_SELCHANGE.
        SendMessageW(tab,WM_LBUTTONDOWN,MK_LBUTTON,point);
        SendMessageW(tab,WM_LBUTTONUP,0,point);
        PumpPaintMessages();
        Require(TabCtrl_GetCurSel(tab)==index,"Native tab did not select requested page");
        HWND shown=index?extended:native, hidden=index?native:extended;
        Require(IsWindowVisible(shown) && !IsWindowVisible(hidden),"Both panes visible or selected pane hidden");
        CheckVisibleGroupBackground(shown,page.host,index && profiles?4:3);
    }
    Require(page.Control(IDD_EDITION_RENDERER_PANE)==native && page.Control(IDD_EDITION_EXTENDED_PANE)==extended,
        "Tab switching recreated pane HWNDs");
    std::cout << "PASS: 40 real tab selections; visible pane/DC clipping and blank group pixels.\n";
}

void Run(HMODULE module, bool profiles) {
    {
        Page page(module);
        Require(page.Visible(IDC_CHECK1) && !page.Visible(IDC_VIDEO_ENHANCEMENT),"Renderer tab initial visibility");
        const HWND native=page.Control(IDC_CHECK1), extended=page.Control(IDC_MEMORY_COLOR_VALUE);
        const HWND nativePane=page.Control(IDD_EDITION_RENDERER_PANE), extendedPane=page.Control(IDD_EDITION_EXTENDED_PANE);
        Require(nativePane && extendedPane && nativePane!=extendedPane && GetParent(native)==nativePane &&
            GetParent(extended)==extendedPane && GetParent(nativePane)==page.window && GetParent(extendedPane)==page.window,
            "Separate child-dialog ownership missing");
        Require(!GetDlgItem(page.window,IDC_CHECK1) && !GetDlgItem(page.window,IDC_MEMORY_COLOR_VALUE),"Controls still flat");
        auto requirePane = [&](bool edition) {
            HWND shown=edition?extendedPane:nativePane;
            HWND hidden=edition?nativePane:extendedPane;
            Require((GetWindowLongPtrW(shown,GWL_STYLE)&WS_VISIBLE)!=0 &&
                (GetWindowLongPtrW(hidden,GWL_STYLE)&WS_VISIBLE)==0,
                "Tab switch left overlapping panes visible");
            Require(GetWindow(page.window,GW_CHILD)==shown,"Active pane not above tab and sibling");
            Require(page.Visible(IDC_CHECK1)==!edition && page.Visible(IDC_VIDEO_ENHANCEMENT)==edition,
                "Control visibility after tab switch");
        };
        requirePane(false);
        for(int i=0;i<100;++i) {
            page.Tab(1); requirePane(true);
            page.Tab(0); requirePane(false);
        }
        Require(page.Control(IDC_CHECK1)==native && page.Control(IDC_MEMORY_COLOR_VALUE)==extended &&
            page.renderer.settingsWrites==0 && page.renderer.previewWrites==0 && page.renderer.saves==0,
            "Tab switching recreated controls or changed renderer settings");
        page.Tab(1);
        Require(!page.Visible(IDC_CHECK1) && page.Visible(IDC_VIDEO_ENHANCEMENT),"Extended tab visibility");
        page.Slider(IDC_MEMORY_COLOR,50);
        Require(page.renderer.memoryColor==50 && !page.renderer.adaptiveTone && !page.renderer.videoEnhancement && page.renderer.saves==0,"Memory independent preview");
        page.Slider(IDC_SKIN_TONE,75);
        Require(page.renderer.skinTone==75 && page.renderer.memoryColor==50,"Skin independent preview");
        page.Slider(IDC_ADAPTIVE_TONE,60);
        Require(page.renderer.adaptiveTone==60 && !page.renderer.videoEnhancement && page.renderer.saves==0,"AT independent preview");
        page.Slider(IDC_VIDEO_ENHANCEMENT,100);
        Require(page.renderer.videoEnhancement==100 && page.renderer.saves==0,"Color preview saved or failed");
        Require(page.Numeric(IDC_MEMORY_COLOR_VALUE)==50,"Slider edit synchronization");
        page.Edit(IDC_MEMORY_COLOR_VALUE,L"57");
        Require(page.renderer.memoryColor==57 && SendMessageW(page.Control(IDC_MEMORY_COLOR),TBM_GETPOS,0,0)==57,"Edit slider synchronization");
        page.Edit(IDC_MEMORY_COLOR_VALUE,L"999");
        Require(page.renderer.memoryColor==100 && page.Numeric(IDC_MEMORY_COLOR_VALUE)==100,"Edit range clamp");
        page.Edit(IDC_MEMORY_COLOR_VALUE,L"");
        Require(page.renderer.memoryColor==100 && page.Numeric(IDC_MEMORY_COLOR_VALUE)==100,"Empty edit rollback");
        page.Edit(IDC_MEMORY_COLOR_VALUE,L"50");
        page.Click(IDC_ENHANCEMENT_MASTER,false);
        Require(!page.renderer.enhancementEnabled && page.renderer.memoryColor==50 && page.Numeric(IDC_MEMORY_COLOR_VALUE)==50 && !IsWindowEnabled(page.Control(IDC_MEMORY_COLOR_VALUE)),"Master off destroyed or failed to disable values");
        page.Tab(0); page.Tab(1);
        Require(!page.renderer.enhancementEnabled,"Tab changed master");
        page.Click(IDC_ENHANCEMENT_MASTER,true);
        Require(page.renderer.enhancementEnabled && IsWindowEnabled(page.Control(IDC_MEMORY_COLOR_VALUE)),"Master restore failed");
        Require(page.Visible(IDC_VR_GROUP)==profiles,"VR group visibility");
        Require(page.renderer.memoryColor==50 && page.renderer.skinTone==75,"Tab switch lost preview");
        if (profiles) {
            page.Edit(IDC_JVR_VERTICAL_VALUE,L"-999");
            Require(page.renderer.settings.iJvrVertical==JVR_VERTICAL_MIN,"Signed VR clamp");
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
        page.Slider(IDC_VIDEO_ENHANCEMENT,0);
        page.Slider(IDC_ADAPTIVE_TONE,0);
        page.Slider(IDC_MEMORY_COLOR,0);
        page.Slider(IDC_SKIN_TONE,0);
        if (profiles) page.Slider(IDC_JVR_VERTICAL, -20);
        page.Click(IDC_ENHANCEMENT_MASTER,false);
        page.Cancel();
        Require(page.renderer.enhancementEnabled,"Cancel did not restore master");
        Require(page.renderer.memoryColor==50 && page.renderer.skinTone==75 && page.renderer.adaptiveTone==60 && page.renderer.videoEnhancement==100 && settings.iJvrVertical == (profiles ? 15 : 17), "Cancel did not restore applied preview");
    }
    {
        Page page(module);
        page.Tab(1); page.Slider(IDC_VIDEO_ENHANCEMENT,100);
        page.Deactivate(); page.Activate();
        page.Cancel();
        Require(!page.renderer.videoEnhancement, "Reactivation lost VE rollback checkpoint");
    }
    {
        Page page(module);
        page.renderer.saveResult = E_FAIL;
        page.Tab(1); page.Slider(IDC_VIDEO_ENHANCEMENT,100);
        Require(page.object->Apply() == E_FAIL, "Apply hid save failure");
        page.Cancel();
        Require(!page.renderer.videoEnhancement, "Failed save advanced VE rollback checkpoint");
    }
    {
        Page page(module);
        page.Tab(1); page.Slider(IDC_VIDEO_ENHANCEMENT,100);
        SendMessageW(page.Control(IDC_ZIPDEPTH_MODE), CB_SETCURSEL, -1, 0);
        Require(page.object->Apply() == E_INVALIDARG && page.renderer.saves == 0, "Invalid selection was committed");
        page.Cancel();
    }
    {
        Page page(module);
        page.Tab(1); page.Slider(IDC_MEMORY_COLOR,25);
        page.Tab(0); page.Click(IDC_BUTTON1,false);
        Require(page.renderer.memoryColor==25,"Native default changed Edition strength");
        page.Tab(1); page.Click(IDC_BUTTON1,false);
        Require(page.renderer.memoryColor==100 && page.renderer.skinTone==100 && page.renderer.videoEnhancement==100 && page.renderer.adaptiveTone==100,"Edition default did not restore strengths");
        page.Cancel();
    }
    {
        Page page(module);
        page.Tab(1);
        page.Edit(IDC_VIDEO_ENHANCEMENT_VALUE,L"43",false);
        Check(page.object->Apply(),"Apply pending numeric edit");
        Require(page.renderer.videoEnhancement==43,"Apply lost pending edit");
        page.Click(IDC_ENHANCEMENT_MASTER,false);
        Check(page.object->Apply(),"Apply master off");
        page.Click(IDC_ENHANCEMENT_MASTER,true);
        page.Cancel();
        Require(!page.renderer.enhancementEnabled && page.renderer.videoEnhancement==43,"Cancel lost applied off master or strengths");
    }
    if (!profiles) {
        Page page(module);
        page.Tab(1);
        page.Click(IDC_BUTTON1, false);
        Check(page.object->Apply(), "Cannot apply defaults");
        Require(page.renderer.settings.bJvrFlatView && page.renderer.settings.iJvrVertical == 17 &&
            page.renderer.settings.iJvrEye == 1, "Defaults changed hidden JVR settings");
    }
    {
        Page page(module);
        page.Click(IDC_CHECK1, false);
        Require(!IsWindowEnabled(page.Control(IDC_MFG_MULTIPLIER)) &&
            !IsWindowEnabled(page.Control(IDC_DLSS_SR_SOURCE_LIMIT)), "D3D11 gate lost");
    }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 3 || (argc == 4 && lstrcmpW(argv[3],L"--visual-tabs")==0),
            "Usage: EditionPageIntegration.exe <filter.ax> <profiles:0|1> [--visual-tabs]");
        if(argc==4) SetProcessDPIAware();
        Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization failed");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
        Require(InitCommonControlsEx(&controls) != FALSE, "Common controls initialization failed");
        HMODULE module = LoadLibraryW(argv[1]);
        Require(module != nullptr, "Cannot load filter directly");
        if(argc==4) RunVisibleTabs(module,argv[2][0] == L'1');
        else Run(module, argv[2][0] == L'1');
        FreeLibrary(module);
        CoUninitialize();
        if(argc==3) std::cout << "PASS: real Edition page preview/apply/cancel, save failure, reactivation, selection validation, D3D11 gate.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
