// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Edition/SvdLutStrength.h"
#include "Edition/SvdLutInput.h"
#include "Edition/VideoEnhancementStartup.h"
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <new>
#include <utility>
#include "Helper.h"
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"
#include "DLSS/NgxRuntime.h"
#include "DLSS/SvdLutShader.h"
#include "DLSS/PerceptualColorShader.h"
#include "Edition/SvdLutTemporalStatistics.h"
#include "Edition/SvdLutB2Luminance.h"
#include "Edition/SvdLutB2Refined.h"
#include "Edition/SvdLutB2AdaptiveLuma.h"
#include "Shaders.h"
#include "Times.h"

namespace {
HRESULT CompileSvdLutOutputShader(const char* source, const D3D_SHADER_MACRO* defines, ID3DBlob** code)
{
    return CompileShader(std::string(SvdLutAdaptiveLumaShader) + source + SvdLutOutputShader, defines, "ps_5_0", code);
}
}

HRESULT svdlut::CreateGradeShader(ID3D11Device* device, ID3D11PixelShader** shader)
{
    CComPtr<ID3DBlob> code;
    const auto hr = CompileSvdLutOutputShader(SvdLutShader, nullptr, &code);
    if (FAILED(hr)) return hr;
    return device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader);
}

// [VE-08 | 2026-11-24 17:09 +08:00] Prepare device-only shaders on the existing model worker.
// The render/presenter threads never call D3DCompile for Video Enhancement.
void svdlut::PrepareVideoEnhancementShaders(ID3D11Device* device, VideoEnhancementShaders& shaders, const std::atomic_bool& cancelled)
{
    const D3D_SHADER_MACRO b[] = {{"P1_RAW_B", "1"}, {nullptr, nullptr}};
    const auto prepare = [&](auto& output, const char* source, const D3D_SHADER_MACRO* defines, bool grade) {
        if (cancelled.load(std::memory_order_acquire)) return;
        CComPtr<ID3DBlob> code;
        const auto hr = grade ? CompileSvdLutOutputShader(source, defines, &code) :
            CompileShader(source, defines, "ps_5_0", &code);
        if (SUCCEEDED(hr) && !cancelled.load(std::memory_order_acquire)) device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
            nullptr, output.GetAddressOf());
    };
    prepare(shaders.thumbnail, SvdLutThumbnailShader, nullptr, false);
    prepare(shaders.fused, PerceptualColorShader, b, true);
    prepare(shaders.chroma, SvdLutB2MemoryShader, nullptr, true);
    prepare(shaders.luma, SvdLutB2LuminanceShader, nullptr, true);
    prepare(shaders.standalone, PerceptualColorShader, nullptr, true);
}

namespace {
constexpr auto ThumbSize = svdlut::InputSize;
static_assert(ThumbSize == svdlut::input::Size);
constexpr auto ThumbFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr unsigned SignatureSize = 8;
using Signature = std::array<float, SignatureSize * SignatureSize * 3>;

std::unique_ptr<svdlut::Image> ReadSvdLutImage(const uint8_t* data, size_t stride)
{
    auto image = std::unique_ptr<svdlut::Image>(new (std::nothrow) svdlut::Image);
    if (!image) return {};
    if (!svdlut::input::ReadPlanar(data, stride, *image)) return {};
    return image;
}

bool SupportedSdrFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

Signature ReadSignature(const uint8_t* data, size_t stride)
{
    Signature signature{};
    constexpr unsigned block = ThumbSize / SignatureSize;
    constexpr unsigned step = 4;
    constexpr float normalized = 1.0f / ((block / step) * (block / step));
    for (unsigned y = 0; y < ThumbSize; y += step) {
        for (unsigned x = 0; x < ThumbSize; x += step) {
            const auto tile = (y / block) * SignatureSize + x / block;
            for (unsigned c = 0; c < 3; ++c) {
                signature[tile * 3 + c] += svdlut::input::Code(data, stride, x + step / 2, y + step / 2, c) * normalized;
            }
        }
    }
    return signature;
}

bool ThumbnailCut(const Signature& previous, const Signature& current)
{
    constexpr auto tiles = SignatureSize * SignatureSize;
    std::array<float, 3> meanShift{};
    unsigned changed = 0, stronglyChanged = 0;
    for (unsigned i = 0; i < tiles; ++i) {
        float distance = 0;
        for (unsigned c = 0; c < 3; ++c) {
            const auto shift = current[i * 3 + c] - previous[i * 3 + c];
            distance += std::abs(shift);
            meanShift[c] += shift;
        }
        changed += distance > 3 * 0.16f;
        stronglyChanged += distance > 3 * 0.27f;
    }
    const auto globalShift = std::abs(meanShift[0]) + std::abs(meanShift[1]) + std::abs(meanShift[2]);
    // Motion across a few regions should leave the grade alone. Require broad
    // image changes and a color shift, or a very strong spatial change.
    return stronglyChanged >= tiles * 7 / 8 ||
        (changed >= tiles * 3 / 4 && globalShift > 3 * tiles * 0.11f);
}

}

// [VE-08 | 2026-11-24 17:09 +08:00] Read-only admission; do not train/modify display pacing,
// the resize recovery arm, MFG bank ownership, or Completed Buffer targets.
bool CDX11VideoProcessor::VideoEnhancementMayStart(bool buffered)
{
    const bool running = buffered && m_pFilter->m_filterState == State_Running;
    const bool mfgActive = m_mfgEffectiveMultiplier.load(std::memory_order_relaxed) >= 2;
    // Reject blocked startup before scanning the ready queue. The actual
    // multiplier, not the UI request, determines whether MFG is bypassed.
    if (m_pFilter->m_bFlushing || m_extendedStartupPending || CompletedGeometryChanging() ||
        (running && !m_completedLast)) return false;
    CRefTime stream;
    const bool clock = running && mfgActive && SUCCEEDED(m_pFilter->StreamTime(stream));
    const auto ready = clock ? GetCompletedBufferTime(stream) : CompletedBufferTime{};
    return edition::ve::MayStart(false, false, false, running, bool(m_completedLast), mfgActive,
        clock, ready.ready, ready.readyFrames);
}

float CDX11VideoProcessor::VideoEnhancementFade(REFERENCE_TIME pts) const
{
    const auto start = m_svdLutRawTest.activationPts;
    return start == INVALID_TIME || pts == INVALID_TIME ? 0.0f : edition::ve::Fade(pts, start);
}

void CDX11VideoProcessor::ResetSvdLutHistory()
{
    // Seek, flush and geometry changes invalidate all temporal LUT state.
    ResetSvdLutTemporalQueue();
}

void CDX11VideoProcessor::ReleaseSvdLut(bool fast)
{
    ResetSvdLutHistory();
    ReleaseSvdLutRawTest(fast);
    m_perceptual.constants.Release();
    m_perceptual.sampler.Release();
    m_perceptual.standalone.Release();
    m_perceptual.rawB.Release();
    m_perceptual.triedStandalone = m_perceptual.triedB = false;
    m_perceptual.failedStandalone = m_perceptual.failedB = false;
    m_perceptual.draws = m_perceptual.depthUsed = m_perceptual.depthFallback = 0;
    m_svdLut.picture.Release();
    m_svdLutThumbnailShader.Release();
    m_svdLutThumbnailConstants.Release();
}

std::wstring CDX11VideoProcessor::SvdLutStatus() const
{
    if (!m_pFilter->m_videoEnhancement.load()) return L"Off";
    const auto& b = m_svdLutRawTest;
    if (b.model.GetState() == svdlut::ModelWorker::State::Failed) return b.model.Failure();
    if (b.failed) return L"Unavailable";
    if (b.active) return L"Active";
    if (!b.attempted) return L"Waiting for SDR video";
    return b.model.GetState() == svdlut::ModelWorker::State::Ready ? L"Analyzing video" : L"Loading DirectML";
}

float CDX11VideoProcessor::BindSvdLutOutputDither(ID3D11Texture2D* target, bool enabled)
{
    float q = 0.0f;
    if (enabled && m_bUseDither && m_TexDither.pShaderResource && target) {
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        if (SupportedSdrFormat(desc.Format))
            q = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1023.0f : 255.0f;
    }
    ID3D11ShaderResourceView* view = q > 0.0f ? m_TexDither.pShaderResource.p : nullptr;
    m_pDeviceContext->PSSetShaderResources(4, 1, &view);
    return q;
}

HRESULT CDX11VideoProcessor::RenderSvdLutThumbnail(const Tex2D_t& source, ID3D11Texture2D* target,
    const CRect& sample)
{
    if (!target || !source.pShaderResource || sample.IsRectEmpty() ||
        sample.left < 0 || sample.top < 0 || sample.right > LONG(source.desc.Width) ||
        sample.bottom > LONG(source.desc.Height)) return E_INVALIDARG;
    if (!m_svdLutThumbnailShader) {
        const auto* prepared = m_svdLutRawTest.model.PreparedShaders();
        if (!prepared || !prepared->thumbnail) return E_FAIL;
        m_svdLutThumbnailShader = prepared->thumbnail.Get();
    }
    if (!m_svdLutThumbnailConstants) {
        D3D11_BUFFER_DESC desc{};
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.ByteWidth = 4 * sizeof(float);
        const auto hr = m_pDevice->CreateBuffer(&desc, nullptr, &m_svdLutThumbnailConstants);
        if (FAILED(hr)) return hr;
    }
    const float rect[4] = {float(sample.left), float(sample.top), float(sample.Width()), float(sample.Height())};
    m_pDeviceContext->UpdateSubresource(m_svdLutThumbnailConstants, 0, nullptr, rect, 0, 0);
    const CRect dst(0, 0, ThumbSize, ThumbSize);
    return TextureCopyRect(source, target, sample, dst, m_svdLutThumbnailShader,
        m_svdLutThumbnailConstants, 0, false);
}

HRESULT CDX11VideoProcessor::EnsureSvdLutPicture(DXGI_FORMAT format, UINT width, UINT height)
{
    auto& picture = m_svdLut.picture;
    const bool sameFormat = picture.pTexture && picture.desc.Format == format;
    if (sameFormat && picture.desc.Width >= width && picture.desc.Height >= height) return S_OK;

    const auto& display = m_pFilter->m_DisplayConfig;
    const UINT capacityWidth = (std::max)({width, UINT(display.width), sameFormat ? picture.desc.Width : 0u});
    const UINT capacityHeight = (std::max)({height, UINT(display.height), sameFormat ? picture.desc.Height : 0u});
    Tex2D_t replacement;
    auto hr = replacement.Create(m_pDevice, format, capacityWidth, capacityHeight, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr) && (capacityWidth != width || capacityHeight != height))
        hr = replacement.Create(m_pDevice, format, width, height, Tex2D_DefaultShaderRTarget);
    if (SUCCEEDED(hr)) picture = std::move(replacement);
    return hr;
}

// [VE-07 | 2026-11-24 16:39:20 +08:00] Single temporal B + P1 composition path.
HRESULT CDX11VideoProcessor::DrawCompletedSvdLut(const CompletedFrame& frame, ID3D11Texture2D* target,
    const CRect& picture, bool geometryChanging)
{
    auto& b = m_svdLutRawTest;
    b.active = false;
    if (!m_pFilter->m_videoEnhancement.load()) return S_FALSE;
    if (b.activationPts == INVALID_TIME && !VideoEnhancementMayStart(true)) return S_FALSE;
    const auto hr = DrawCompletedSvdLutRaw(frame, target, picture, geometryChanging);
    if (hr == S_OK) return hr;
    // Do not introduce perceptual colour first and then abruptly add the LUT.
    // A genuinely unavailable model can still use the existing safe fallback.
    if (b.activationPts == INVALID_TIME && !b.failed &&
        b.model.GetState() != svdlut::ModelWorker::State::Failed) return S_FALSE;
    return DrawCompletedPerceptual(frame, target, picture, geometryChanging);
}

void CDX11VideoProcessor::DrawSvdLut(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp)
{
    auto& b = m_svdLutRawTest;
    b.active = false;
    if (!m_pFilter->m_videoEnhancement.load()) return;
    if (b.activationPts == INVALID_TIME && !VideoEnhancementMayStart(false)) return;
    DrawSvdLutRaw(target, picture, timestamp);
    if (!b.active && (b.activationPts != INVALID_TIME || b.failed ||
        b.model.GetState() == svdlut::ModelWorker::State::Failed))
        DrawPerceptual(target, picture, timestamp);
}

// P1: no new model, readback, asynchronous task, or full-size colour pass
// when the temporal grade is already drawing its LUT. Standalone mode replaces the normal
// completed-frame blit where geometry permits.
bool CDX11VideoProcessor::PerceptualEnabled() const
{
    return m_pFilter->m_videoEnhancement.load();
}

HRESULT CDX11VideoProcessor::PreparePerceptualShader(int mode)
{
    auto& e = m_perceptual;
    if (!e.constants) {
        D3D11_BUFFER_DESC cb{};
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.ByteWidth = 24 * sizeof(float);
        const auto hr = m_pDevice->CreateBuffer(&cb, nullptr, &e.constants);
        if (FAILED(hr)) return hr;
    }
    if (!e.sampler) {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        const auto hr = m_pDevice->CreateSamplerState(&desc, &e.sampler);
        if (FAILED(hr)) return hr;
    }
    bool& tried = mode == 2 ? e.triedB : e.triedStandalone;
    bool& failed = mode == 2 ? e.failedB : e.failedStandalone;
    auto& shader = mode == 2 ? e.rawB : e.standalone;
    if (shader) return S_OK;
    if (tried) return E_FAIL;
    const auto* prepared = m_svdLutRawTest.model.PreparedShaders();
    if (!prepared) return S_FALSE;
    tried = true;
    shader = mode == 2 ? prepared->fused.Get() : prepared->standalone.Get();
    failed = !shader;
    return failed ? E_FAIL : S_OK;
}

HRESULT CDX11VideoProcessor::ApplyPerceptual(const Tex2D_t& source, ID3D11Texture2D* target,
    const CRect& area, const CRect& visible, CPoint offset, REFERENCE_TIME pts,
    int mode, ID3D11ShaderResourceView* lut, std::array<float, 3> tone, bool useTone,
    float blend, bool allowDepth)
{
    if (!PerceptualEnabled() || !target || (mode != 0 && mode != 2) || (mode == 2 && !lut)) return S_FALSE;
    if (PreparePerceptualShader(mode) != S_OK) return S_FALSE;
    auto& e = m_perceptual;
    auto* shader = mode == 2 ? e.rawB.p : e.standalone.p;
    if (!shader) return S_FALSE;

    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* previousDepth = nullptr;
    float confidence = 0.0f, rotated = 0.0f, previousRotated = 0.0f;
    float historyWeight = 0.0f;
    // The source depth is only safe at exact picture geometry. There is no
    // arbitrary SR/MFG/letterbox coordinate guess and NEVER extra inference.
    if (allowDepth && pts != INVALID_TIME && (UsesZipDepthSr() || UsesZipDepthMfg()) &&
        visible == area && offset == CPoint(0, 0) &&
        m_zipDepth.MatchesColorPicture(visible.Width(), visible.Height())) {
        const auto estimate = m_zipDepth.ForFrame(pts);
        const auto age = pts - estimate.pts;
        if (estimate.view && estimate.pts != INVALID_TIME && age >= 0 && age < 1'900'000) {
            // Fade stale depth softly, never snap colour at one timestamp.
            confidence = 0.50f * (std::min)(1.0f,
                float(1'900'000 - age) / 1'000'000.0f);
            depth = estimate.view;
            rotated = estimate.rotated ? 1.0f : 0.0f;
            // Reuse a previous cached map from the SAME scene and capture size.
            // No extra model invocation, GPU pass or CPU readback. The pixel
            // shader can damp depth disagreement at a small sampling cost.
            if (estimate.pts > 0) {
                const auto previous = m_zipDepth.ForFrame(estimate.pts - 1);
                if (previous.view && previous.pts >= 0 &&
                    estimate.pts - previous.pts <= 1'500'000 &&
                    previous.rotated == estimate.rotated) {
                    previousDepth = previous.view;
                    previousRotated = previous.rotated ? 1.0f : 0.0f;
                    historyWeight = 1.0f;
                }
            }
        }
    }
    const float cb[24] = {
        float(visible.left), float(visible.top),
        float(visible.Width() - 1), float(visible.Height() - 1),
        blend, float(offset.x), float(offset.y), rotated,
        tone[0], tone[1], tone[2], confidence,
        m_pFilter->m_videoEnhancement.load() ? 1.0f : 0.0f,
        m_pFilter->m_videoEnhancement.load() ? 1.0f : 0.0f,
        useTone ? 1.0f : 0.0f,
        mode == 2 && useTone ? 1.0f : 0.0f,
        historyWeight, previousRotated,
        0.01f, mode == 2 ? VideoEnhancementFade(pts) :
            (m_svdLutRawTest.activationPts == INVALID_TIME ? 1.0f : VideoEnhancementFade(pts)),
        // DepthTuning.w is final whole-result activation, not LUT strength.
        BindSvdLutOutputDither(target, mode != 0), 0, 0, 0
    };
    CRect input(area);
    input.OffsetRect(offset);
    m_pDeviceContext->UpdateSubresource(e.constants, 0, nullptr, cb, 0, 0);
    ID3D11ShaderResourceView* noView = nullptr;
    ID3D11SamplerState* noSampler = nullptr;
    m_pDeviceContext->PSSetShaderResources(1, 1, mode ? &lut : &noView);
    m_pDeviceContext->PSSetShaderResources(2, 1, depth ? &depth : &noView);
    m_pDeviceContext->PSSetShaderResources(3, 1, previousDepth ? &previousDepth : &noView);
    m_pDeviceContext->PSSetSamplers(1, 1, &e.sampler.p);
    m_pDeviceContext->PSSetSamplers(2, 1, depth ? &e.sampler.p : &noSampler);
    const auto hr = TextureCopyRect(source, target, input, area, shader, e.constants, 0, false);
    m_pDeviceContext->PSSetShaderResources(1, 1, &noView);
    m_pDeviceContext->PSSetShaderResources(2, 1, &noView);
    m_pDeviceContext->PSSetShaderResources(3, 1, &noView);
    m_pDeviceContext->PSSetShaderResources(4, 1, &noView);
    m_pDeviceContext->PSSetSamplers(1, 1, &noSampler);
    m_pDeviceContext->PSSetSamplers(2, 1, &noSampler);
    if (SUCCEEDED(hr)) {
        ++e.draws;
        if (depth && confidence > .001f) ++e.depthUsed;
        else ++e.depthFallback;
    }
    return hr;
}

HRESULT CDX11VideoProcessor::DrawCompletedPerceptual(const CompletedFrame& frame,
    ID3D11Texture2D* target, const CRect& picture, bool geometryChanging)
{
    if (!PerceptualEnabled() || !target ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return S_FALSE;
    D3D11_TEXTURE2D_DESC d{};
    target->GetDesc(&d);
    const CRect area(0, 0, d.Width, d.Height);
    CRect visible;
    if (!SupportedSdrFormat(d.Format) || !visible.IntersectRect(picture, area) ||
        visible.Width() < 2 || visible.Height() < 2) return S_FALSE;
    if (frame.picture == picture && frame.image.desc.Width == d.Width &&
        frame.image.desc.Height == d.Height && frame.image.desc.Format == d.Format &&
        frame.image.pShaderResource) {
        return ApplyPerceptual(frame.image, target, area, visible, CPoint(0, 0), frame.pts,
            0, nullptr, {1.0f, 1.0f, 1.0f}, false, 1.0f,
            !geometryChanging && frame.picture == area);
    }
    // Same scalable fallback as temporal SVDLUT: draw the normal picture once into
    // an existing reusable texture, then apply the enhancement in the final draw.
    if (FAILED(EnsureSvdLutPicture(d.Format, d.Width, d.Height))) return S_FALSE;
    const auto drawn = DrawCompletedVideo(frame, m_svdLut.picture.pTexture, picture,
        geometryChanging, &area);
    if (drawn != S_OK) return drawn;
    return ApplyPerceptual(m_svdLut.picture, target, area, visible, CPoint(0, 0), frame.pts, 0);
}

void CDX11VideoProcessor::DrawPerceptual(ID3D11Texture2D* target,
    const CRect& picture, REFERENCE_TIME pts)
{
    if (!PerceptualEnabled() || !target ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return;
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    CRect visible;
    if (!SupportedSdrFormat(desc.Format) ||
        !visible.IntersectRect(picture, CRect(0, 0, desc.Width, desc.Height)) ||
        visible.Width() < 2 || visible.Height() < 2 ||
        FAILED(EnsureSvdLutPicture(desc.Format, visible.Width(), visible.Height()))) return;
    // Legacy pre-MFG path must first copy the target to a shader-readable
    // surface; this copy is not needed on the direct completed-video path.
    const D3D11_BOX box{UINT(visible.left), UINT(visible.top), 0,
        UINT(visible.right), UINT(visible.bottom), 1};
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_pDeviceContext->CopySubresourceRegion(m_svdLut.picture.pTexture, 0, 0, 0, 0, target, 0, &box);
    ApplyPerceptual(m_svdLut.picture, target, visible, visible,
        CPoint(-visible.left, -visible.top), pts, 0);
}

#include "Edition/SvdLutRawMode.inc"
