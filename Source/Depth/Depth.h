// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DepthGpu.h"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

// One source-frame capture and GPU model per D3D11 renderer.
class CDepth final
{
public:
    static constexpr float NormalizedDepthSpan = 0.12f;
    struct Estimate {
        ID3D11ShaderResourceView* view = nullptr;
        int64_t pts = -1;
        bool rotated = false;
    };
    struct CacheStats {
        unsigned cached = 0;
        unsigned ahead = 0;
        unsigned capacity = 0;
    };
    // RendererLock-only diagnostic views of existing CPU metadata. No GPU query.
    const auto& CachedResultPts() const noexcept { return m_resultPts; }
    const auto& SceneCutPts() const noexcept { return m_sceneCuts; }
    struct Counters {
        uint64_t captures = 0;
        uint64_t jobs = 0;
        uint64_t maps = 0;
        uint64_t lateMaps = 0;
        uint64_t srEvaluations = 0; // successful SR evaluations with model depth
        uint64_t mfgSubmissions = 0; // accepted MFG submissions with model depth
        uint64_t srDepthRequests = 0; // all successful SR evaluations requesting depth
        uint64_t mfgDepthRequests = 0; // all accepted MFG submissions requesting depth
        uint64_t lastSrUseTick = 0;
        uint64_t lastMfgUseTick = 0;
        double inferenceMs = 0.0;
        CDepthGpu::Timings gpuTimings;
        double mapsPerSecond = 0.0;
        uint64_t mapRateSampleTick = 0;
        int64_t resultAge = 0;
        int64_t rawMfgCheckAge = -1; // media age at the most recent RAW MFG depth check
        const wchar_t* rawMfgCheck = L"not checked";
    };
    static constexpr float OutputDepthSpan = 0.30f;
    bool DrawEstimate(ID3D11DeviceContext1* context, ID3D11Texture2D* output,
        ID3D11RenderTargetView* target, const RECT& picture, SIZE modelSize,
        int64_t pts, float frameTimeMs, bool inverted, ID3D11ShaderResourceView* active = nullptr);
    SIZE TensorSize() const { return m_gpu.ModelSize(); }
    // Depth was captured for precisely this source-content size. The LUT
    // consumer must additionally require a matching output rectangle/frame.
    bool MatchesColorPicture(LONG width, LONG height) const noexcept {
        return width > 0 && height > 0 && m_modelSize.cx == width && m_modelSize.cy == height &&
            (m_capturePicture.right - m_capturePicture.left) == width &&
            (m_capturePicture.bottom - m_capturePicture.top) == height;
    }
    // The normalized model describes the full source content, irrespective
    // of an ordinary scale used for a producer snapshot. No new state.
    bool MatchesColorSource(LONG width,LONG height) const noexcept {
        return m_initialized && width>0 && height>0 && m_modelSize.cx==width && m_modelSize.cy==height &&
            m_capturePicture.right>m_capturePicture.left && m_capturePicture.bottom>m_capturePicture.top;
    }
    uint64_t ColorGeneration() const noexcept { return m_generation; }
    bool SameColorScene(int64_t a,int64_t b) const { return a>=0 && b>=0 && SceneStart(a)==SceneStart(b); }
    static constexpr int64_t MaxEstimateAge = 2'000'000; // 200 ms in media time

    void Prepare(ID3D11Device1* device);
    bool Loading() const { return m_gpu.Loading(); }
    bool Unavailable() const { return m_device && !m_gpu.CanStart(); }
    void Update(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts);
    bool PrefetchNv12(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11Texture2D* frame, const RECT& picture, bool fullRange, int64_t pts);
    void PlaybackFrame(int64_t pts, int64_t duration);
    void StopLookahead() { m_lookahead = false; m_playbackPts = -1; ClearOriginalSelection(); }
    // Harvest on the renderer's serialized source/presentation paths.
    void PollCompleted();
    // Give an already submitted original a bounded opportunity to finish.
    // Called only from original-frame producer paths, never generated phases.
    void AwaitOriginal(int64_t pts, int64_t duration, HANDLE cancelEvent);
    Estimate ForFrame(int64_t pts) const;
    Estimate LatestPreview() const;
    CacheStats GetCacheStats(int64_t playbackPts) const;
    const Counters& GetCounters() const { return m_counters; }
    void RecordSrEvaluation(bool usedDepth) {
        ++m_counters.srDepthRequests;
        if (usedDepth) {
            ++m_counters.srEvaluations;
            m_counters.lastSrUseTick = GetTickCount64();
        }
    }
    void RecordMfgSubmission(bool usedDepth) {
        ++m_counters.mfgDepthRequests;
        if (usedDepth) {
            ++m_counters.mfgSubmissions;
            m_counters.lastMfgUseTick = GetTickCount64();
        }
    }
    bool CoolingDown(uint64_t now) const { return now < m_retryAt; }
    void MarkSceneCut(int64_t pts);
    void ResetHistory();
    void Stop(bool fastShutdown = false);
    const wchar_t* Status() const;

private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    int64_t SceneStart(int64_t pts) const;
    Estimate EstimateAtOrBefore(int64_t pts) const;
    // Serialized original producer only. Pin a cache entry by its PTS, not a
    // naked SRV: the existing capture fence orders GPU reads before reuse.
    void ClearOriginalSelection() noexcept {
        m_selectedSourcePts = m_selectedDepthPts = -1;
    }
    Estimate SelectOriginal(int64_t pts);
    void RecordResultAge(int64_t age, uint64_t now);
    bool Initialize(ID3D11Device1* device);
    void Capture(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts,
        ID3D11ShaderResourceView* chroma = nullptr, bool fullRange = false);
    Ptr<ID3D11Device1> m_device;
    Ptr<ID3DDeviceContextState> m_state;
    Ptr<ID3D11VertexShader> m_vertex;
    Ptr<ID3D11PixelShader> m_captureShader, m_captureNv12Shader, m_depthShader;
    Ptr<ID3D11SamplerState> m_sampler;
    Ptr<ID3D11Buffer> m_constants;
    std::array<int64_t, CDepthGpu::Slots> m_resultPts = [] {
        std::array<int64_t, CDepthGpu::Slots> slots{};
        slots.fill(-1);
        return slots;
    }();
    std::array<int64_t, 8> m_sceneCuts = [] {
        std::array<int64_t, 8> cuts{};
        cuts.fill(-1);
        return cuts;
    }();
    CDepthGpu m_gpu;
    bool m_initialized = false;
    bool m_lookahead = false;
    int64_t m_playbackPts = -1;
    int64_t m_previousPlaybackPts = -1;
    int64_t m_originalDuration = 0;
    int64_t m_waitedOriginalPts = -1;
    int64_t m_selectedSourcePts = -1;
    int64_t m_selectedDepthPts = -1; // -1 is an intentional neutral decision
    SIZE m_modelSize{};
    SIZE m_captureSize{};
    RECT m_capturePicture{};
    int64_t m_lastSourcePts = -1;
    int64_t m_lastSubmittedPts = -1;
    uint64_t m_mapRateTick = 0;
    uint64_t m_mapRateBase = 0;
    uint64_t m_mapRateLastPoll = 0;
    uint64_t m_generation = 1;
    unsigned m_consecutiveLate = 0;
    uint64_t m_retryAt = 0;
    Counters m_counters;
};
