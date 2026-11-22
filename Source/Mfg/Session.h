// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include "FlowMotionCheck.h"
class CZipDepth;

namespace mfg {
// One source frame owns an independent reference to its full-resolution D3D11
// snapshot. The MFG bank may be recycled, but this source must not be overwritten
// until the presenter releases the final reference.
struct OriginalSnapshot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    // Only Native D3D12 sessions allocate shareable snapshots. The opened
    // native-device alias is owned by this snapshot for its entire queue lifetime.
    Microsoft::WRL::ComPtr<ID3D12Resource> nativeView;
};
struct DepthInput {
    CZipDepth& source;
    RECT picture;
    SIZE modelSize;
    bool captureSource = true;
    bool applyDepth = true;
    bool adaptive = false;
};
struct FrameGeometry {
    RECT picture; // video rectangle in MFG input pixels
    RECT crop;    // decoder source crop
    RECT present; // video rectangle in presentation pixels
};
struct Batch {
    uint64_t serial=0;
    unsigned generated=0;
    int64_t previous=0,current=0;
    std::shared_ptr<OriginalSnapshot> original;
};
// Borrowed resources: caller must hold RendererLock while using them.
// SRVs are created once when the bank is allocated, not on every Present.
struct FrameSurface {
    ID3D11Texture2D* texture=nullptr;
    ID3D11ShaderResourceView* view=nullptr;
    bool pending=false; // GPU producer has not signalled completion yet
};
struct MotionOverlay {
    ID3D11ShaderResourceView* view = nullptr;
    RECT picture{};
    ID3D11ShaderResourceView* active = nullptr;
};
struct DepthPreview {
    ID3D11ShaderResourceView* view = nullptr;
    RECT picture{};
    SIZE size{};
};
class Session {
    struct Impl;
    struct WarmupJob;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<WarmupJob> warmup_;
    std::wstring status_;
    std::atomic<bool> failed_{false};
    static std::mutex s_retiredWarmupMutex;
    static std::vector<std::unique_ptr<WarmupJob>> s_retiredWarmups;
    static void ReapRetiredWarmups();
    static void RetireWarmup(std::unique_ptr<WarmupJob> job);
public:
    Session();~Session();
    // Preroll-only initialization: allocate NGX/flow/GPU resources without
    // submitting a frame, advancing history, or starting the presenter.
    bool Warmup(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11Texture2D* image, unsigned multiplier,
        bool nativeSharedOriginals = false);
    // True when the session is ready or initialization failed permanently.
    bool PollWarmup(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11Texture2D* image);
    bool Generate(ID3D11Device1* device,ID3D11DeviceContext1* context,ID3D11Texture2D* image,
        unsigned multiplier,int64_t timestamp,Batch& batch,
        bool nativeSharedOriginals = false, bool inspectMotion = false, const DepthInput* depth = nullptr,
        const FrameGeometry* geometry = nullptr);
    FlowMotion TakeFlowMotion(int64_t pts);
    // x1 Auto: keep the source frame in the SAME queued presenter without NGX.
    bool CaptureOriginal(ID3D11Texture2D* image,int64_t timestamp,Batch& batch,const DepthInput* depth = nullptr);
    // Non-blocking completion and NGX validity check for a synthetic phase.
    // No presentation lock or CPU fence wait is performed here.
    bool Ready(uint64_t serial,unsigned index) const;
    FrameSurface Frame(uint64_t serial,unsigned index,int64_t current = -1) const;
    MotionOverlay OverlayMotion(uint64_t serial, int64_t pts) const;
    DepthPreview OverlayDepth(uint64_t serial, int64_t pts) const;
    // Mark the generated bank as consumed AFTER D3D11 presentation work is
    // enqueued. The producer waits for this fence before reusing its outputs.
    void MarkConsumed(uint64_t serial);
    bool AsyncProducer() const;
    // Reserve the bank actually used by the next Generate() call. Called
    // under RendererLock while accepting a CompletedFrame source sample.
    std::optional<uint64_t> ReserveNextBankSerial(bool requirePresenterReadComplete,
        std::span<const uint64_t> protectedGroups) const;
    void ClearNextBankReservation();
    bool GenerationPending(uint64_t serial) const;
    uint64_t BusyFallbacks() const;
    unsigned BankCount() const;
    // Current runtime ceiling, which can decrease after an NGX rejection.
    unsigned MaxAvailableMultiplier() const;
    SIZE InputSize() const;
    SIZE GeneratedSize() const;
    bool UsesZipDepth() const;
    std::wstring WorkDescription() const;
    void Reset();
    void FastShutdown();
    void ResetHistory();
    void ResetDepthHistory();
    bool Failed() const {return failed_;}
    const std::wstring& Status() const {return status_;}
};
}
