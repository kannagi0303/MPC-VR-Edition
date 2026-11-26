// SPDX-License-Identifier: GPL-3.0-or-later
// The runner extracts the actual production method into a generated include.
// Only its GPU/backend collaborators are substituted; no algorithm is copied here.
#include <windows.h>
#include <strmif.h>
#include <atltypes.h>
#include <vector>
#include <string>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <optional>
#include <d3d9.h>
#include <VersionHelpers.h>
#include "Edition/VideoFrame.h"
#include "Edition/PlaybackCancellation.h"
#include "IVideoRenderer.h"

// Exercise the actual native threshold dispatch together with the Edition
// product policy. Driver calls only record their argument; no GPU is created.
constexpr UINT PCIV_NVIDIA = 0x10de, PCIV_INTEL = 0x8086;
struct CD3D11VP {
    void* m_pVideoContext = this;
    UINT m_VendorId = PCIV_NVIDIA, m_srcWidth = 1920, m_srcHeight = 1080;
    int calls = 0;
    bool enabled = false;
    HRESULT SetSuperResNvidia(bool value) { ++calls; enabled = value; return S_OK; }
    HRESULT SetSuperResIntel(bool) { throw std::runtime_error("Edition allowed Intel VSR"); }
    HRESULT SetSuperRes(int mode);
    std::optional<HRESULT> ApplyEditionSuperResPolicy(int mode);
};
#include "SuperResolutionPolicy.generated.inc"

static void TestSuperResolutionPolicy() {
    unsigned cases = 0;
    for (UINT vendor : {PCIV_NVIDIA, PCIV_INTEL, 0u}) {
        for (int mode : {SUPERRES_Disable, SUPERRES_SD, SUPERRES_1080p, SUPERRES_Always, SUPERRES_DLSS_SR}) {
            for (bool context : {false, true}) {
                CD3D11VP vp;
                vp.m_VendorId = vendor;
                if (!context) vp.m_pVideoContext = nullptr;
                const HRESULT result = vp.SetSuperRes(mode);
                const bool supported = context && vendor == PCIV_NVIDIA;
                if (result != (!context ? E_ABORT : supported ? S_OK : E_NOT_SET) ||
                    vp.calls != int(supported) ||
                    (supported && vp.enabled != (mode == SUPERRES_1080p || mode == SUPERRES_Always))) {
                    throw std::runtime_error("Changed native VSR threshold/product policy");
                }
                ++cases;
            }
        }
    }
    CD3D11VP portrait;
    portrait.m_srcWidth = 1080; portrait.m_srcHeight = 1920;
    if (portrait.SetSuperRes(SUPERRES_1080p) != S_OK || !portrait.enabled)
        throw std::runtime_error("Changed portrait VSR threshold");
    std::cout << "PASS: " << cases + 1 << " actual native VSR threshold and Edition product-policy cases.\n";
}

// Non-owning stand-ins used only by the preparation method's target selection.
struct __declspec(uuid("665CC9E8-6334-4B58-99B4-77B399D70071")) ID3D11Texture2D : IUnknown {
    STDMETHODIMP QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
};
template<class T> struct CComPtr {
    T* p = nullptr;
    T** operator&() { return &p; }
    CComPtr& operator=(T* value) { p = value; return *this; }
};
uint64_t GetPreciseTick() { static uint64_t tick = 100; return ++tick; }
#define DLogIf(...) ((void)0)
#define DLog(...) ((void)0)

struct Tex2D_t { struct { UINT Width, Height; } desc; };
struct CDX11VideoProcessor {
    std::vector<std::string> events;
    struct History {
        std::vector<std::string>* events;
        const char* name;
        bool active = false;
        bool failed = false;
        bool Active() const { return active; }
        bool Failed() const { return failed; }
        void ResetHistory() { events->push_back(name); }
    } m_dlssSR{&events, "sr-history"}, m_mfg{&events, "mfg-history"};
    struct RepeatTiming {
        std::vector<std::string>* events;
        void ClearEvidence() { events->push_back("repeat-evidence"); }
    } m_mfgRepeatTiming{&events};
    struct VideoProcessor {
        bool ready = false;
        bool IsReady() const { return ready; }
    } m_D3D11VP;
    struct Filter {
        struct { bool bWaitForExtendedStartup = false; } m_Sets;
        FILTER_STATE m_filterState = State_Running;
        mpcvr::edition::PlaybackCancellation m_playbackCancel;
    } filter;
    Filter* m_pFilter = &filter;
    std::atomic<unsigned> m_mfgMultiplier{0};
    bool m_bVPScalingUseShaders = false;
    bool bypass = false, srSucceeds = false;
    bool receivedSecond = false;
    REFERENCE_TIME receivedTimestamp = 0;
    const Tex2D_t* receivedPrepared = nullptr;
    Tex2D_t enhanced{{1920,1080}};
    bool MfgSourceMeetsDisplayRate() const { return bypass; }
    void StopMfg(bool release) {
        if (release) throw std::runtime_error("Source switch released MFG session");
        events.push_back("stop-mfg");
    }
    void ResetMfgFrameAnalysis() { events.push_back("frame-analysis"); }
    Tex2D_t* TryDlssSR(const CRect&, const CRect&, bool second, REFERENCE_TIME timestamp,
        bool* overlay, const Tex2D_t* prepared) {
        events.push_back("try-sr");
        receivedSecond = second; receivedTimestamp = timestamp; receivedPrepared = prepared;
        if (overlay && srSucceeds) *overlay = true;
        return srSucceeds ? &enhanced : nullptr;
    }
    const Tex2D_t* SelectEditionProcessingSource(const CRect&, const CRect&, bool, bool,
        REFERENCE_TIME, bool*, const Tex2D_t*, CRect&);
    struct CompletedFrame {
        REFERENCE_TIME pts = 0, end = 0, due = 0;
        bool videoOnly = false, projected = false;
        struct { ID3D11Texture2D* pTexture = nullptr; } image;
        CRect picture{0,0,1280,720};
    };
    std::optional<CompletedFrame> m_completedLast, m_completedTarget;
    bool m_mfgInputModeKnown = false, m_completingSample = false;
    bool m_mfgPresentFailed = false, pendingFrames = false, projectJvr = false;
    REFERENCE_TIME m_mfgAutoEpoch = 0, m_mfgAutoLastPts = 0;
    REFERENCE_TIME m_rtStart = 10, m_sampleDuration = 333667, m_rtVideoDelay = 100000;
    CSize m_processingSize{0,0};
    int m_FieldDrawn = 0;
    CRect m_srcRect{13,11,653,371}, m_windowRect{0,0,1920,1080}, m_videoRect{0,0,1920,1080};
    HRESULT resizeResult = S_OK, projectionResult = S_OK, completedResult = S_OK;
    struct SwapChain {
        std::vector<std::string>* events;
        HRESULT result = S_OK;
        ID3D11Texture2D image;
        HRESULT GetBuffer(UINT, REFIID, void** out) {
            events->push_back("back-buffer");
            if (SUCCEEDED(result)) *out = &image;
            return result;
        }
    } swapChain{&events};
    SwapChain* m_pDXGISwapChain1 = &swapChain;
    bool completedBackBufferInvalidated = false;
    void InvalidateCompletedBackBuffer() { completedBackBufferInvalidated = true; }
    HRESULT ApplyPendingWindowResize() { events.push_back("resize"); return resizeResult; }
    void UpdateProcessingResolution() { events.push_back("resolution"); }
    bool CanProjectJvr() const { return projectJvr; }
    bool CompletedFramesPending() const { return pendingFrames; }
    HRESULT PresentCompletedVideo(const CompletedFrame&) { events.push_back("completed-present"); return completedResult; }
    CRect ProcessingSourceRect(bool projected) const { return projected ? CRect(0,0,1104,621) : m_srcRect; }
    CRect PictureForPlayback(bool) const { return m_videoRect; }
    CRect ProcessingRect() const { return CRect(0,0,m_processingSize.cx,m_processingSize.cy); }
    HRESULT PrepareJvrFrame(const CRect&, bool second) {
        receivedSecond = second;
        events.push_back("projection"); return projectionResult;
    }
    std::optional<HRESULT> PrepareEditionRender(edition::VideoFrame&, CComPtr<ID3D11Texture2D>&, uint64_t&, HRESULT&);
};

#include "EditionProcessingSource.generated.inc"
#include "EditionRenderPreparation.generated.inc"

void TestRenderPreparation() {
    auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
    auto run = [](CDX11VideoProcessor& processor, edition::VideoFrame& frame, CComPtr<ID3D11Texture2D>& buffer) {
        uint64_t tick = 0;
        HRESULT status = S_OK;
        return processor.PrepareEditionRender(frame, buffer, tick, status);
    };
    {
        CDX11VideoProcessor processor;
        processor.filter.m_playbackCancel.Cancel();
        edition::VideoFrame frame{.field=1, .frameStartTime=10};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        require(result && *result == S_OK && processor.events.empty() && !buffer.p &&
            !processor.completedBackBufferInvalidated, "Cancelled timeline launched renderer work");
    }
    {
        CDX11VideoProcessor processor;
        processor.resizeResult = E_FAIL;
        edition::VideoFrame frame{.field=1, .frameStartTime=10};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        require(result && *result == E_FAIL && !buffer.p && processor.events == std::vector<std::string>{"resize"},
            "Resize failure continued into native rendering");
    }
    for (HRESULT completed : {S_OK, E_FAIL}) {
        CDX11VideoProcessor processor;
        processor.m_completedLast.emplace();
        processor.completedResult = completed;
        edition::VideoFrame frame{.field=0, .frameStartTime=10};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        require(result && *result == completed && !buffer.p &&
            processor.events == std::vector<std::string>{"resolution","completed-present"},
            "Completed redraw result, including S_OK, did not end Render");
    }
    {
        CDX11VideoProcessor processor;
        processor.m_mfgMultiplier = 7;
        processor.m_mfgInputModeKnown = true;
        processor.m_processingSize = CSize(1920,1080);
        edition::VideoFrame frame{.field=0, .frameStartTime=10};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        require(result && *result == S_OK && !buffer.p, "Presenter-owned redraw acquired another buffer");
    }
    for (bool failed : {false,true}) {
        CDX11VideoProcessor processor;
        processor.m_mfgMultiplier = 7;
        processor.m_mfgInputModeKnown = true;
        processor.m_mfg.failed = failed;
        processor.bypass = true;
        edition::VideoFrame frame{.field=1, .frameStartTime=10};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        std::vector<std::string> expected{"stop-mfg"};
        if (!failed) expected.push_back("mfg-history");
        expected.insert(expected.end(), {"resize","resolution","back-buffer"});
        require(!result && !frame.mfgEnabledForRate && !processor.m_mfgInputModeKnown &&
            processor.m_mfgAutoEpoch == -1 && processor.m_mfgAutoLastPts == -1 && processor.events == expected,
            "Refresh-rate bypass changed history/reset order");
    }
    for (bool project : {false,true}) for (bool projectionFails : {false,true}) {
        CDX11VideoProcessor processor;
        processor.projectJvr = project;
        processor.projectionResult = projectionFails ? E_FAIL : S_OK;
        processor.m_completedLast.emplace();
        edition::VideoFrame frame{.field=1, .frameStartTime=100};
        CComPtr<ID3D11Texture2D> buffer;
        uint64_t tick = 0;
        HRESULT status = E_FAIL;
        const auto result = processor.PrepareEditionRender(frame, buffer, tick, status);
        require(!result && status == S_OK && buffer.p == &processor.swapChain.image && tick > 0 && processor.completedBackBufferInvalidated &&
            frame.projected == (project && !projectionFails) && frame.sourceReady == (!project || !projectionFails) &&
            frame.frameSource == (project && !projectionFails ? CRect(0,0,1104,621) : processor.m_srcRect) &&
            !processor.m_completedLast, "Projection fallback changed source/target or stale-frame clearing");
    }
    {
        CDX11VideoProcessor processor;
        processor.swapChain.result = E_FAIL;
        edition::VideoFrame frame{.field=1, .frameStartTime=100};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        require(result && *result == E_FAIL && !buffer.p, "Back-buffer failure continued into composition");
    }
    {
        CDX11VideoProcessor processor;
        processor.projectJvr = true;
        processor.m_completingSample = true;
        processor.m_completedTarget.emplace();
        processor.m_completedTarget->videoOnly = true;
        ID3D11Texture2D image;
        processor.m_completedTarget->image.pTexture = &image;
        edition::VideoFrame frame{.field=2, .frameStartTime=100};
        CComPtr<ID3D11Texture2D> buffer;
        const auto result = run(processor, frame, buffer);
        require(!result && buffer.p == &image && frame.second && frame.completedVideoOnly && frame.projected &&
            processor.m_completedTarget->pts == 100 && processor.m_completedTarget->end == 100 + processor.m_sampleDuration &&
            processor.m_completedTarget->due == 100 + processor.m_rtVideoDelay &&
            processor.events == std::vector<std::string>{"projection"}, "Completed target lost PTS/field/ownership");
    }
    std::cout << "PASS: 12 actual Render preparation cases, including early S_OK, resize/target failure, projection fallback and completed-frame PTS.\n";
}

int TestSplitPack();
int TestDepthJobQueue();
int TestDepthStreamContract();
int TestDmlGraphDiagnostics();
int TestResizeBankRecovery();
int TestSvdLutReferenceInput();
int TestSvdLutAdaptiveLuma();
int TestSvdLutReferenceShaders();
int TestVideoEnhancementStartup();
int TestAdaptiveToneShot();
void TestCompletedBufferCalculation();
void TestDxgiStatisticsAudit();
void TestOptionalColorWork();
void TestPresentationProducerFairness();
void TestResizeGeometryAdmission();
void TestSourceToneRouting();
void TestDepthPlaybackStatistics();
void TestDepthPreviewAndConsume();
void TestPerceptualLifecycle();
void TestEnhancementMasterAndLayout();
void TestSourceToneBeforeSR();
void TestAdaptiveToneContinuity();
void TestAdaptiveToneComposition();
void TestDepthRawPipeline();

int main() {
    std::cout << std::unitbuf;
    try {
        unsigned combinations = 0;
        for (unsigned mask = 0; mask < 256; ++mask) {
            for (bool second : {false,true}) {
                for (bool previousScaling : {false,true}) {
                    CDX11VideoProcessor processor;
                    const bool allow = mask & 1, active = mask & 2, succeeds = mask & 4;
                    const bool mfg = mask & 8, bypass = mask & 16, wait = mask & 32;
                    const bool prepared = mask & 64, vp = mask & 128;
                    processor.m_dlssSR.active = active;
                    processor.srSucceeds = succeeds;
                    processor.m_mfgMultiplier = mfg ? 7u : 0u;
                    processor.bypass = bypass;
                    processor.filter.m_Sets.bWaitForExtendedStartup = wait;
                    processor.m_D3D11VP.ready = vp;
                    processor.m_bVPScalingUseShaders = previousScaling;
                    Tex2D_t projected{{1104,621}};
                    const Tex2D_t* supplied = prepared ? &projected : nullptr;
                    const CRect source(13,11,653,371), target(0,0,1920,1080);
                    CRect effectiveSource = source;
                    bool overlay = false;
                    constexpr REFERENCE_TIME pts = 123456789;
                    const auto selected = processor.SelectEditionProcessingSource(source, target, second,
                        allow, pts, &overlay, supplied, effectiveSource);
                    const bool enhanced = allow && succeeds;
                    auto require = [mask](bool condition, const char* message) {
                        if (!condition) throw std::runtime_error(std::to_string(mask) + ": " + message);
                    };
                    require(selected == (enhanced ? &processor.enhanced : supplied), "Wrong source priority/fallback");
                    require(effectiveSource == (enhanced ? CRect(0,0,1920,1080) : source), "Changed source crop");
                    require(processor.m_bVPScalingUseShaders ==
                        (!enhanced && prepared ? vp : previousScaling), "Changed VP scaling ownership");
                    require(overlay == enhanced, "Lost overlay result");
                    if (allow) require(processor.receivedSecond == second && processor.receivedTimestamp == pts &&
                        processor.receivedPrepared == supplied, "Lost SR input timing or prepared texture");
                    std::vector<std::string> expected;
                    if (allow) expected.push_back("try-sr");
                    if (allow && !enhanced && active) expected.push_back("sr-history");
                    if (allow && active != enhanced && mfg && !bypass) {
                        if (wait) expected.push_back("stop-mfg");
                        else { expected.push_back("frame-analysis"); expected.push_back("repeat-evidence"); }
                        expected.push_back("mfg-history");
                    }
                    require(processor.events == expected, "Changed SR/MFG transition order");
                    ++combinations;
                }
            }
        }
        std::cout << "PASS: " << combinations << " actual processing-source routing and SR/MFG transition cases.\n";
        TestRenderPreparation();
        TestSuperResolutionPolicy();
        for (auto test : {TestSplitPack, TestDepthJobQueue, TestDepthStreamContract, TestDmlGraphDiagnostics, TestResizeBankRecovery,
            TestSvdLutReferenceInput, TestSvdLutAdaptiveLuma, TestSvdLutReferenceShaders, TestVideoEnhancementStartup, TestAdaptiveToneShot}) {
            if (test() != 0) throw std::runtime_error("Native regression test failed");
        }
        TestCompletedBufferCalculation();
        TestDxgiStatisticsAudit();
        TestOptionalColorWork();
        TestPresentationProducerFairness();
        TestResizeGeometryAdmission();
        TestSourceToneRouting();
        TestDepthPlaybackStatistics();
        TestDepthPreviewAndConsume();
        TestPerceptualLifecycle();
        TestEnhancementMasterAndLayout();
        TestSourceToneBeforeSR();
        TestAdaptiveToneContinuity();
        TestAdaptiveToneComposition();
        TestDepthRawPipeline();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
