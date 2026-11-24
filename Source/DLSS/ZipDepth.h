// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ZipDepthGpu.h"
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>

// One source-frame capture and GPU model per D3D11 renderer.
class CZipDepth final
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
    struct Counters {
        uint64_t captures = 0;
        uint64_t jobs = 0;
        uint64_t maps = 0;
        uint64_t lateMaps = 0;
        uint64_t srEvaluations = 0;
        uint64_t mfgSubmissions = 0;
        uint64_t lastSrUseTick = 0;
        uint64_t lastMfgUseTick = 0;
        double inferenceMs = 0.0;
        CZipDepthGpu::Timings gpuTimings;
        double mapsPerSecond = 0.0;
        int64_t resultAge = 0;
    };
    SIZE TensorSize() const { return m_gpu.ModelSize(); }
    // Depth was captured for precisely this source-content size. The LUT
    // consumer must additionally require a matching output rectangle/frame.
    bool MatchesColorPicture(LONG width, LONG height) const noexcept {
        return width > 0 && height > 0 && m_modelSize.cx == width && m_modelSize.cy == height &&
            (m_capturePicture.right - m_capturePicture.left) == width &&
            (m_capturePicture.bottom - m_capturePicture.top) == height;
    }
    static constexpr int64_t MaxEstimateAge = 2'000'000; // 200 ms in media time

    void Prepare(ID3D11Device1* device);
    bool Loading() const { return m_gpu.Loading(); }
    void Update(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts);
    bool PrefetchNv12(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11Texture2D* frame, const RECT& picture, bool fullRange, int64_t pts);
    void PlaybackFrame(int64_t pts, int64_t duration);
    void StopLookahead() { m_lookahead = false; m_playbackPts = -1; }
    // Harvest on the renderer's serialized source/presentation paths.
    void PollCompleted();
    Estimate ForFrame(int64_t pts) const;
    Estimate LatestPreview() const;
    CacheStats GetCacheStats(int64_t playbackPts) const;
    const Counters& GetCounters() const { return m_counters; }
    void RecordSrEvaluation() {
        ++m_counters.srEvaluations;
        m_counters.lastSrUseTick = GetTickCount64();
    }
    void RecordMfgSubmission() {
        ++m_counters.mfgSubmissions;
        m_counters.lastMfgUseTick = GetTickCount64();
    }
    bool CoolingDown(uint64_t now) const { return now < m_retryAt; }
    void MarkSceneCut(int64_t pts);
    void ResetHistory();
    const wchar_t* ModelPrecision() const { return m_gpu.ModelPrecision(); }
    void Stop(bool fastShutdown = false);
    uint64_t Generation() const { return m_generation; }
    const wchar_t* Status() const;

private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    friend class CZipDepthHistory;
    int64_t SceneStart(int64_t pts) const;
    Estimate EstimateAtOrBefore(int64_t pts) const;
    void RecordResultAge(int64_t age, uint64_t now);
    bool Initialize(ID3D11Device1* device);
    void Capture(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts,
        ID3D11ShaderResourceView* chroma = nullptr, bool fullRange = false);
    Ptr<ID3D11Device1> m_device;
    Ptr<ID3DDeviceContextState> m_state;
    Ptr<ID3D11VertexShader> m_vertex;
    Ptr<ID3D11PixelShader> m_captureShader, m_captureNv12Shader, m_temporalShader, m_expandDepthShader;
    Ptr<ID3D11SamplerState> m_sampler;
    Ptr<ID3D11Buffer> m_constants;
    std::array<int64_t, CZipDepthGpu::Slots> m_resultPts = [] {
        std::array<int64_t, CZipDepthGpu::Slots> slots{};
        slots.fill(-1);
        return slots;
    }();
    std::array<int64_t, 8> m_sceneCuts = [] {
        std::array<int64_t, 8> cuts{};
        cuts.fill(-1);
        return cuts;
    }();
    CZipDepthGpu m_gpu;
    bool m_initialized = false;
    bool m_lookahead = false;
    int64_t m_playbackPts = -1;
    SIZE m_modelSize{};
    SIZE m_captureSize{};
    RECT m_capturePicture{};
    int64_t m_lastSourcePts = -1;
    int64_t m_lastSubmittedPts = -1;
    uint64_t m_mapRateTick = 0;
    uint64_t m_mapRateBase = 0;
    uint64_t m_generation = 1;
    unsigned m_consecutiveLate = 0;
    uint64_t m_retryAt = 0;
    Counters m_counters;
};

// A consumer reprojects the shared estimate in its own frame order and
// coordinate space. The result is written into its existing NGX depth target.
class CZipDepthHistory final
{
public:
    static constexpr float OutputDepthSpan = 0.30f;
    struct FramePair {
        int64_t previousPts;
        ID3D11ShaderResourceView* currentLuma;
        ID3D11ShaderResourceView* previousLuma;
        bool requireAdjacentEstimate;
    };

    bool Draw(ID3D11Device1* device, ID3D11DeviceContext1* context,
        const CZipDepth& source, ID3D11Texture2D* output, ID3D11RenderTargetView* target,
        ID3D11ShaderResourceView* motion, const RECT& picture, SIZE modelSize,
        int64_t pts, float frameTimeMs, bool inverted, const FramePair* pair = nullptr,
        ID3D11ShaderResourceView* active = nullptr, ID3D11ShaderResourceView* previousActive = nullptr);
    void Reset() { m_estimatePts = m_historyPts = -1; }

private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Grid {
        Ptr<ID3D11Texture2D> texture;
        Ptr<ID3D11ShaderResourceView> view;
        Ptr<ID3D11RenderTargetView> target;
    };
    std::array<Grid, 2> m_grids;
    unsigned m_gridIndex = 0;
    SIZE m_gridSize{}, m_size{};
    RECT m_picture{};
    int64_t m_estimatePts = -1;
    int64_t m_historyPts = -1;
    uint64_t m_generation = 0;
    bool m_failed = false;
};
