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
    struct Estimate {
        ID3D11ShaderResourceView* view = nullptr;
        int64_t pts = -1;
        SIZE modelSize{};
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
    static constexpr unsigned Size = CZipDepthGpu::Size;
    static constexpr int64_t MaxEstimateAge = 2'000'000; // 200 ms in media time

    void Prepare(ID3D11Device1* device);
    void Update(ID3D11Device1* device, ID3D11DeviceContext1* context,
        ID3D11ShaderResourceView* source, const RECT& picture, SIZE modelSize, int64_t pts);
    // Harvest on the renderer's serialized source/presentation paths.
    void PollCompleted();
    Estimate ForFrame(int64_t pts) const;
    Estimate LatestPreview() const;
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
    void Stop();
    uint64_t Generation() const { return m_generation; }
    const wchar_t* Status() const;

private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    friend class CZipDepthHistory;
    int64_t SceneStart(int64_t pts) const;
    Estimate EstimateAtOrBefore(int64_t pts) const;
    void RecordResultAge(int64_t age, uint64_t now);
    bool Initialize(ID3D11Device1* device);
    Ptr<ID3D11Device1> m_device;
    Ptr<ID3DDeviceContextState> m_state;
    Ptr<ID3D11VertexShader> m_vertex;
    Ptr<ID3D11PixelShader> m_captureShader, m_temporalShader;
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
    SIZE m_modelSize{};
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
    bool Draw(ID3D11Device1* device, ID3D11DeviceContext1* context,
        const CZipDepth& source, ID3D11Texture2D* output, ID3D11RenderTargetView* target,
        ID3D11ShaderResourceView* motion, const RECT& picture, SIZE modelSize,
        int64_t pts, float frameTimeMs, bool inverted);
    void Reset() { m_estimatePts = -1; }

private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Texture2D> m_history;
    Ptr<ID3D11ShaderResourceView> m_historyView;
    SIZE m_size{};
    RECT m_picture{};
    int64_t m_estimatePts = -1;
    uint64_t m_generation = 0;
    bool m_failed = false;
};
