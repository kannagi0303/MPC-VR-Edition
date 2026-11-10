// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <cstdint>
#include <atomic>
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
class Session {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::wstring status_;
    std::atomic<bool> failed_{false};
public:
    Session();~Session();
    bool Generate(ID3D11Device1* device,ID3D11DeviceContext1* context,ID3D11Texture2D* image,
        unsigned multiplier,int64_t timestamp,const std::wstring& directory,Batch& batch,
        bool nativeSharedOriginals = false);
    // x1 Auto: keep the source frame in the SAME queued presenter without NGX.
    bool CaptureOriginal(ID3D11Texture2D* image,int64_t timestamp,Batch& batch);
    // Non-blocking completion and NGX validity check for a synthetic phase.
    // No presentation lock or CPU fence wait is performed here.
    bool Ready(uint64_t serial,unsigned index) const;
    FrameSurface Frame(uint64_t serial,unsigned index) const;
    // Mark the generated bank as consumed AFTER D3D11 presentation work is
    // enqueued. The producer waits for this fence before reusing its outputs.
    void MarkConsumed(uint64_t serial);
    bool AsyncProducer() const;
    uint64_t BusyFallbacks() const;
    unsigned BankCount() const;
    // NGX-advertised ceiling after the first successful session creation.
    unsigned MaxAvailableMultiplier() const;
    std::wstring WorkDescription() const;
    void Reset();
    void ResetHistory();
    bool Failed() const {return failed_;}
    const std::wstring& Status() const {return status_;}
};
}
