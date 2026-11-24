// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
#include "Edition/SvdLutStrength.h"
#include "Edition/SvdLutInput.h"
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

namespace {
constexpr REFERENCE_TIME CaptureInterval = 125 * 10000;
constexpr REFERENCE_TIME FadeInDuration = 500 * 10000;
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

float InitialGradeBlend(REFERENCE_TIME timestamp, REFERENCE_TIME start)
{
    if (timestamp <= start) return 0.0f;
    const auto elapsed = static_cast<uint64_t>(timestamp) - static_cast<uint64_t>(start);
    const float t = float((std::min)(elapsed, uint64_t(FadeInDuration))) / FadeInDuration;
    return t * t * (3.0f - 2.0f * t);
}
}

void CDX11VideoProcessor::SetSvdLutStrength(int strength)
{
    auto& lut = m_svdLut;
    const bool wasEnabled = lut.strength != 0;
    lut.strength = strength;
    if (wasEnabled == (strength != 0)) return;
    if (!strength && (lut.failed || lut.model.GetState() == svdlut::ModelWorker::State::Failed))
        ReleaseSvdLut(true);
    else
        ResetSvdLutHistory();
}

void CDX11VideoProcessor::ResetSvdLutHistory()
{
    auto& lut = m_svdLut;
    lut.model.Invalidate();
    lut.input.reset();
    lut.scenes.clear();
    lut.hasSignature = false;
    lut.active = false;
    lut.waitingForBuffer = false;
    lut.captured = lut.inference = lut.transitionStart = lut.nextCapture = INVALID_TIME;
    lut.possibleCut = INVALID_TIME;
    lut.staging.Release();
    if (m_pFilter->m_svdLutRaw.load()) {
        // Only Temporal B owns asynchronous readbacks/held LUTs. Flush, seek
        // and geometry changes must invalidate them as well as the frame ring;
        // otherwise a previous segment can grade the first new frames.
        if (m_pFilter->m_svdLutTemporal.load()) {
            ResetSvdLutTemporalQueue();
        } else {
            // Keep RAW B's original reset behavior unchanged.
            m_svdLutRawTest.frames = {};
            m_svdLutRawTest.frameSlot = 0;
            m_svdLutRawTest.lastWindow = 0;
        }
    }
}

void CDX11VideoProcessor::ReleaseSvdLut(bool fast)
{
    ResetSvdLutHistory();
    ReleaseSvdLutRawTest(fast);
    m_perceptual.constants.Release();
    m_perceptual.sampler.Release();
    m_perceptual.standalone.Release();
    m_perceptual.normalA.Release();
    m_perceptual.rawB.Release();
    m_perceptual.triedStandalone = m_perceptual.triedA = m_perceptual.triedB = false;
    m_perceptual.failedStandalone = m_perceptual.failedA = m_perceptual.failedB = false;
    m_perceptual.draws = m_perceptual.depthUsed = m_perceptual.depthFallback = 0;
    auto& lut = m_svdLut;
    lut.model.Stop(fast || m_fastShutdown);
    lut.attempted = lut.failed = false;
    lut.clearViewSupported = false;
    lut.picture.Release();
    lut.nativeThumbnail.Release();
    m_svdLutThumbnailShader.Release();
    m_svdLutThumbnailConstants.Release();
    lut.thumbnail.Release();
    lut.lutSampler.Release();
    lut.constants.Release();
}

std::wstring CDX11VideoProcessor::SvdLutStatus() const
{
    if (m_pFilter->m_svdLutOff.load()) return L"Off";
    if (m_pFilter->m_svdLutRaw.load()) {
        const auto& b = m_svdLutRawTest;
        if (b.failed) return L"RAW unavailable";
        if (b.model.GetState() == svdlut::ModelWorker::State::Failed) return b.model.Failure();
        if (b.active) return L"RAW active";
        if (!b.attempted) return L"RAW waiting for SDR video";
        return b.model.GetState() == svdlut::ModelWorker::State::Ready ? L"RAW evaluating" : L"RAW loading DirectML";
    }
    const auto& lut = m_svdLut;
    if (lut.failed) return L"Unavailable";
    const auto state = lut.model.GetState();
    if (state == svdlut::ModelWorker::State::Failed) return lut.model.Failure();
    if (lut.active) return L"Active";
    if (lut.waitingForBuffer) return L"Waiting for video buffer";
    if (!lut.attempted) return L"Waiting for SDR video";
    return state == svdlut::ModelWorker::State::Ready ? L"Analyzing video" : L"Loading DirectML model";
}

HRESULT CDX11VideoProcessor::PrepareSvdLutDraw()
{
    auto& lut = m_svdLut;

    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    lut.clearViewSupported = SUCCEEDED(m_pDevice->CheckFeatureSupport(
        D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) && options.ClearView;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    const auto hr = m_pDevice->CreateSamplerState(&sampler, &lut.lutSampler);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC desc{};
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.ByteWidth = 12 * sizeof(float);
    const auto created = m_pDevice->CreateBuffer(&desc, nullptr, &lut.constants);
    if (SUCCEEDED(created) && m_processingSize.cx > 0 && m_processingSize.cy > 0 && SupportedSdrFormat(m_SwapChainFmt)) {
        // Prepare the reusable surface before a resize needs it. If the
        // preferred size cannot be allocated, presentation can request less.
        EnsureSvdLutPicture(m_SwapChainFmt, m_processingSize.cx, m_processingSize.cy);
    }
    return created;
}

bool CDX11VideoProcessor::StartSvdLutModel()
{
    auto& lut = m_svdLut;
    lut.attempted = true;
    CComQIPtr<IDXGIDevice> dxgi(m_pDevice.p);
    CComPtr<IDXGIAdapter> adapter;
    lut.failed = !dxgi || FAILED(dxgi->GetAdapter(&adapter)) ||
        !lut.model.Start(adapter, m_pDevice, NgxRuntime::EditionDir());
    return !lut.failed;
}

HRESULT CDX11VideoProcessor::EnsureSvdLutThumbnails()
{
    auto& lut = m_svdLut;
    const auto hr = lut.thumbnail.CheckCreate(m_pDevice, ThumbFormat,
        ThumbSize, ThumbSize, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr)) return hr;
    return lut.staging.CheckCreate(m_pDevice, ThumbFormat,
        ThumbSize, ThumbSize, Tex2D_StagingRead);
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
        CComPtr<ID3DBlob> code;
        auto hr = CompileShader(SvdLutThumbnailShader, nullptr, "ps_5_0", &code);
        if (FAILED(hr)) return hr;
        hr = m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
            &m_svdLutThumbnailShader);
        if (FAILED(hr)) return hr;
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

bool CDX11VideoProcessor::SvdLutWorkAllowed()
{
    // Optional work must give buffered playback a chance to refill.
    CRefTime stream;
    constexpr REFERENCE_TIME reserve = 100 * 10'000;
    const bool allowed = !m_completingSample ||
        (SUCCEEDED(m_pFilter->StreamTime(stream)) && GetCompletedBufferTime(stream).ready >= reserve);
    m_svdLut.waitingForBuffer = !allowed;
    return allowed;
}

void CDX11VideoProcessor::BeginSvdLutScene(REFERENCE_TIME timestamp)
{
    auto& lut = m_svdLut;
    if (m_pFilter->m_svdLutRaw.load() || lut.failed || timestamp == INVALID_TIME ||
        (!lut.scenes.empty() && timestamp <= lut.scenes.back().start)) return;
    try {
        lut.scenes.push_back({timestamp, false, lut.scenes.empty(), {}});
    } catch (const std::bad_alloc&) {
        lut.failed = true;
        return;
    }
    // Completed playback is capped at 256 frames. Bound outstanding cuts
    // even if presentation has stopped.
    if (lut.scenes.size() > 256) lut.scenes.pop_front();
    lut.input.reset();
    lut.nextCapture = INVALID_TIME;
    lut.possibleCut = INVALID_TIME;
    lut.hasSignature = false;
}

void CDX11VideoProcessor::TakeSvdLutResult()
{
    auto& lut = m_svdLut;
    if (lut.inference != INVALID_TIME) {
        auto result = lut.model.TakeResult();
        if (result) {
            const auto scene = std::find_if(lut.scenes.begin(), lut.scenes.end(),
                [&](const auto& entry) { return entry.start == lut.inference; });
            if (scene != lut.scenes.end()) scene->view = std::move(result);
            lut.inference = INVALID_TIME;
        }
    }
}

void CDX11VideoProcessor::PollSvdLut()
{
    auto& lut = m_svdLut;
    TakeSvdLutResult();
    if (lut.captured != INVALID_TIME) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr = m_pDeviceContext->Map(lut.staging.pTexture, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == S_OK) {
            const auto timestamp = std::exchange(lut.captured, INVALID_TIME);
            if (lut.scenes.empty() || timestamp >= lut.scenes.back().start) {
                const auto* data = static_cast<const uint8_t*>(mapped.pData);
                const auto signature = ReadSignature(data, mapped.RowPitch);
                if (lut.scenes.empty()) {
                    BeginSvdLutScene(timestamp);
                }
                if (lut.hasSignature && ThumbnailCut(lut.signature, signature)) {
                    // A second thumbnail distinguishes a sustained cut from a brief flash.
                    if (lut.possibleCut != INVALID_TIME && !ThumbnailCut(lut.cutSignature, signature)) {
                        BeginSvdLutScene(lut.possibleCut);
                    } else {
                        lut.possibleCut = timestamp;
                        lut.cutSignature = signature;
                    }
                } else {
                    lut.possibleCut = INVALID_TIME;
                }
                if (lut.possibleCut == INVALID_TIME) {
                    lut.signature = signature;
                    lut.hasSignature = true;
                }
                lut.nextCapture = timestamp + CaptureInterval;
                if (!lut.failed && lut.possibleCut == INVALID_TIME && !lut.scenes.back().submitted && !lut.input) {
                    lut.input = ReadSvdLutImage(data, mapped.RowPitch);
                }
            }
            m_pDeviceContext->Unmap(lut.staging.pTexture, 0);
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            lut.failed = true;
            return;
        }
    }
    if (!lut.failed && lut.input && lut.inference == INVALID_TIME && SvdLutWorkAllowed() && lut.model.ReadyForInput()) {
        auto& scene = lut.scenes.back();
        scene.submitted = lut.model.Submit(std::move(lut.input));
        if (scene.submitted) lut.inference = scene.start;
    }
}

void CDX11VideoProcessor::CaptureSvdLut(REFERENCE_TIME timestamp, bool projected, bool second)
{
    if (m_pFilter->m_svdLutOff.load()) return;
    auto& lut = m_svdLut;
    if (m_pFilter->m_svdLutRaw.load() || !lut.strength || lut.failed || timestamp == INVALID_TIME ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return;

    if (!lut.attempted) {
        if (!SvdLutWorkAllowed() || !StartSvdLutModel()) return;
    }
    if (lut.failed || lut.model.GetState() != svdlut::ModelWorker::State::Ready) return;
    PollSvdLut();
    if (lut.failed || lut.captured != INVALID_TIME ||
        (lut.nextCapture != INVALID_TIME && timestamp < lut.nextCapture) || !SvdLutWorkAllowed()) return;

    if (!lut.constants && FAILED(PrepareSvdLutDraw())) {
        lut.failed = true;
        return;
    }

    lut.nextCapture = timestamp + CaptureInterval;
    const CRect thumbnail(0, 0, ThumbSize, ThumbSize);
    if (FAILED(EnsureSvdLutThumbnails())) {
        lut.failed = true;
        return;
    }

    // Sample the decoded picture before SR/MFG, or exactly the projected
    // JVR surface used by ZipDepth. Reduce hardware video directly to a thumbnail.
    CRect sourceRect = ProcessingSourceRect(projected);
    const Tex2D_t* source = &m_TexSrcVideo;
    if (projected) {
        source = &m_jvrFrame;
    } else if (m_D3D11VP.IsReady()) {
        if (FAILED(lut.nativeThumbnail.CheckCreate(m_pDevice, m_D3D11OutputFmt,
                ThumbSize, ThumbSize, Tex2D_DefaultShaderRTarget)) ||
            FAILED(D3D11VPPass(lut.nativeThumbnail.pTexture, sourceRect, thumbnail, second))) return;
        source = &lut.nativeThumbnail;
        sourceRect = thumbnail;
    } else if (m_PSConvColorData.bEnable) {
        if (FAILED(ConvertColorPass(m_TexConvertOutput.pTexture))) return;
        source = &m_TexConvertOutput;
        sourceRect.SetRect(0, 0, source->desc.Width, source->desc.Height);
    }
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    if (!source->pShaderResource || sourceRect.IsRectEmpty() ||
        FAILED(RenderSvdLutThumbnail(*source, lut.thumbnail.pTexture, sourceRect))) return;
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_pDeviceContext->CopyResource(lut.staging.pTexture, lut.thumbnail.pTexture);
    lut.captured = timestamp;
}

const CDX11VideoProcessor::SvdLutScene* CDX11VideoProcessor::SvdLutSceneForPts(REFERENCE_TIME timestamp)
{
    auto& scenes = m_svdLut.scenes;
    if (scenes.empty() || timestamp < scenes.front().start) return nullptr;
    // Keep the previous shot available for a redraw of the last presented frame.
    while (scenes.size() > 2 && scenes[2].start <= timestamp) scenes.pop_front();
    return scenes.size() > 1 && scenes[1].start <= timestamp ? &scenes[1] : &scenes.front();
}

std::optional<CDX11VideoProcessor::SvdLutGrade> CDX11VideoProcessor::PrepareSvdLutGrade(REFERENCE_TIME timestamp)
{
    auto& lut = m_svdLut;
    lut.active = false;
    if (!lut.strength || lut.failed || timestamp == INVALID_TIME ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return std::nullopt;

    auto* shader = lut.model.Shader();
    if (!shader || !lut.constants) return std::nullopt;
    TakeSvdLutResult();
    const auto* scene = SvdLutSceneForPts(timestamp);
    if (!scene || !scene->view) return std::nullopt;

    if (scene->fadeIn && lut.transitionStart == INVALID_TIME) lut.transitionStart = timestamp;
    const float fade = scene->fadeIn ? InitialGradeBlend(timestamp, lut.transitionStart) : 1.0f;
    return SvdLutGrade{shader, scene->view.Get(), fade * edition::svdstrength::AScale(lut.strength)};
}

HRESULT CDX11VideoProcessor::ApplySvdLutGrade(const SvdLutGrade& grade, const Tex2D_t& source, ID3D11Texture2D* target,
    const CRect& area, const CRect& visible, CPoint sourceOffset)
{
    auto& lut = m_svdLut;
    const float constants[12] = {
        float(visible.left), float(visible.top), float(visible.Width() - 1), float(visible.Height() - 1),
        grade.blend, float(sourceOffset.x), float(sourceOffset.y), 0,
        BindSvdLutOutputDither(target), 0, 0, 0
    };
    CRect input = area;
    input.OffsetRect(sourceOffset);
    m_pDeviceContext->UpdateSubresource(lut.constants, 0, nullptr, constants, 0, 0);
    m_pDeviceContext->PSSetShaderResources(1, 1, &grade.view);
    m_pDeviceContext->PSSetSamplers(1, 1, &lut.lutSampler.p);
    const auto hr = TextureCopyRect(source, target, input, area, grade.shader, lut.constants, 0, false);
    ID3D11ShaderResourceView* noView = nullptr;
    ID3D11SamplerState* noSampler = nullptr;
    m_pDeviceContext->PSSetShaderResources(1, 1, &noView);
    m_pDeviceContext->PSSetShaderResources(4, 1, &noView);
    m_pDeviceContext->PSSetSamplers(1, 1, &noSampler);
    lut.failed = FAILED(hr);
    lut.active = !lut.failed;
    return hr;
}

HRESULT CDX11VideoProcessor::DrawCompletedSvdLut(const CompletedFrame& frame, ID3D11Texture2D* target,
    const CRect& picture, bool geometryChanging)
{
    if (m_pFilter->m_svdLutOff.load())
        return DrawCompletedPerceptual(frame, target, picture, geometryChanging);
    if (m_pFilter->m_svdLutRaw.load()) {
        const auto hr = DrawCompletedSvdLutRaw(frame, target, picture, geometryChanging);
        return hr == S_OK ? hr : DrawCompletedPerceptual(frame, target, picture, geometryChanging);
    }
    const auto grade = PrepareSvdLutGrade(frame.pts);
    if (!grade) return DrawCompletedPerceptual(frame, target, picture, geometryChanging);

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    const CRect area(0, 0, desc.Width, desc.Height);
    CRect visible;
    if (!SupportedSdrFormat(desc.Format) || !visible.IntersectRect(picture, area) ||
        visible.Width() < 2 || visible.Height() < 2) return S_FALSE;

    const auto& source = frame.image.desc;
    if (frame.picture == picture && source.Width == desc.Width && source.Height == desc.Height &&
        source.Format == desc.Format && frame.image.pShaderResource) {
        if (PerceptualEnabled()) {
            const auto fused = ApplyPerceptual(frame.image, target, area, visible, CPoint(0, 0),
                frame.pts, 1, grade->view, {1.0f, 1.0f, 1.0f}, false, grade->blend,
                !geometryChanging && frame.picture == area);
            if (fused == S_OK) return fused;
        }
        return ApplySvdLutGrade(*grade, frame.image, target, area, visible, CPoint(0, 0));
    }

    // Keep the same scaler and black borders. Render its result to a texture
    // that the final grade pass can read directly, without copying the swap chain.
    const auto created = EnsureSvdLutPicture(desc.Format, desc.Width, desc.Height);
    if (FAILED(created)) {
        m_svdLut.failed = true;
        return created;
    }
    const auto& pictureTexture = m_svdLut.picture;
    const auto drawn = DrawCompletedVideo(frame, pictureTexture.pTexture, picture, geometryChanging, &area);
    if (drawn != S_OK) return drawn;
    if (PerceptualEnabled()) {
        const auto fused = ApplyPerceptual(pictureTexture, target, area, visible, CPoint(0, 0),
            frame.pts, 1, grade->view, {1.0f, 1.0f, 1.0f}, false, grade->blend);
        if (fused == S_OK) return fused;
    }
    return ApplySvdLutGrade(*grade, pictureTexture, target, area, visible, CPoint(0, 0));
}

void CDX11VideoProcessor::DrawSvdLut(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp)
{
    if (m_pFilter->m_svdLutOff.load()) {
        DrawPerceptual(target, picture, timestamp);
        return;
    }
    if (m_pFilter->m_svdLutRaw.load()) {
        DrawSvdLutRaw(target, picture, timestamp);
        if (!m_svdLutRawTest.active) DrawPerceptual(target, picture, timestamp);
        return;
    }
    const auto grade = PrepareSvdLutGrade(timestamp);
    if (!grade || !target) {
        DrawPerceptual(target, picture, timestamp);
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    CRect visible;
    if (!SupportedSdrFormat(desc.Format) ||
        !visible.IntersectRect(picture, CRect(0, 0, desc.Width, desc.Height)) ||
        visible.Width() < 2 || visible.Height() < 2) return;

    auto& lut = m_svdLut;
    // The legacy presenter may already have written its picture to the target.
    const auto width = static_cast<UINT>(visible.Width());
    const auto height = static_cast<UINT>(visible.Height());
    if (FAILED(EnsureSvdLutPicture(desc.Format, width, height))) {
        lut.failed = true;
        return;
    }
    const D3D11_BOX box{UINT(visible.left), UINT(visible.top), 0, UINT(visible.right), UINT(visible.bottom), 1};
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_pDeviceContext->CopySubresourceRegion(lut.picture.pTexture, 0, 0, 0, 0, target, 0, &box);
    if (PerceptualEnabled()) {
        const auto fused = ApplyPerceptual(lut.picture, target, visible, visible,
            CPoint(-visible.left, -visible.top), timestamp, 1, grade->view,
            {1.0f, 1.0f, 1.0f}, false, grade->blend);
        if (fused == S_OK) return;
    }
    ApplySvdLutGrade(*grade, lut.picture, target, visible, visible, CPoint(-visible.left, -visible.top));
}


// P1: no new model, readback, asynchronous task, or full-size colour pass
// when A/B is already drawing its LUT. Standalone mode replaces the normal
// completed-frame blit where geometry permits.
bool CDX11VideoProcessor::PerceptualEnabled() const
{
    return m_pFilter->m_preferredColors.load() || m_pFilter->m_skinTones.load();
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
    bool& tried = mode == 1 ? e.triedA : mode == 2 ? e.triedB : e.triedStandalone;
    bool& failed = mode == 1 ? e.failedA : mode == 2 ? e.failedB : e.failedStandalone;
    auto& shader = mode == 1 ? e.normalA : mode == 2 ? e.rawB : e.standalone;
    if (shader) return S_OK;
    if (tried) return E_FAIL;
    tried = true;
    const D3D_SHADER_MACRO a[] = {{"P1_NORMAL_A", "1"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO b[] = {{"P1_RAW_B", "1"}, {nullptr, nullptr}};
    CComPtr<ID3DBlob> code;
    HRESULT hr = CompileSvdLutOutputShader(PerceptualColorShader, mode == 1 ? a : mode == 2 ? b : nullptr, &code);
    if (SUCCEEDED(hr))
        hr = m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
            nullptr, &shader);
    failed = FAILED(hr);
    return hr;
}

HRESULT CDX11VideoProcessor::ApplyPerceptual(const Tex2D_t& source, ID3D11Texture2D* target,
    const CRect& area, const CRect& visible, CPoint offset, REFERENCE_TIME pts,
    int mode, ID3D11ShaderResourceView* lut, std::array<float, 3> tone, bool useTone,
    float blend, bool allowDepth)
{
    if (!PerceptualEnabled() || !target || (mode != 0 && !lut)) return S_FALSE;
    if (FAILED(PreparePerceptualShader(mode))) return S_FALSE;
    auto& e = m_perceptual;
    auto* shader = mode == 1 ? e.normalA.p : mode == 2 ? e.rawB.p : e.standalone.p;
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
        m_pFilter->m_preferredColors.load() ? 1.0f : 0.0f,
        m_pFilter->m_skinTones.load() ? 1.0f : 0.0f,
        useTone ? 1.0f : 0.0f,
        mode == 2 && useTone && m_pFilter->m_svdLutB2Memory.load() &&
            !m_pFilter->m_svdLutR1Gamut.load() && !m_pFilter->m_svdLutM1Skin.load() ? 1.0f : 0.0f,
        historyWeight, previousRotated,
        0.01f, 0.0f, // 1% maximum linear-light distant-scene tonal lift-down.
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
    // Same scalable fallback as SVDLUT A/B: draw the normal picture once into
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
