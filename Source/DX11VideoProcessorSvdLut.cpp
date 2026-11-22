// SPDX-License-Identifier: GPL-3.0-or-later
#include "stdafx.h"
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
#include "Edition/SvdLutTemporalStatistics.h"
#include "Edition/SvdLutB2Luminance.h"
#include "Shaders.h"
#include "Times.h"

HRESULT svdlut::CreateGradeShader(ID3D11Device* device, ID3D11PixelShader** shader)
{
    CComPtr<ID3DBlob> code;
    const auto hr = CompileShader(SvdLutShader, nullptr, "ps_5_0", &code);
    if (FAILED(hr)) return hr;
    return device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader);
}

namespace {
constexpr REFERENCE_TIME CaptureInterval = 125 * 10000;
constexpr REFERENCE_TIME FadeInDuration = 500 * 10000;
constexpr auto ThumbSize = svdlut::InputSize;
constexpr unsigned SignatureSize = 8;
using Signature = std::array<float, SignatureSize * SignatureSize * 3>;

std::unique_ptr<svdlut::Image> ReadSvdLutImage(const uint8_t* data, size_t stride)
{
    auto image = std::unique_ptr<svdlut::Image>(new (std::nothrow) svdlut::Image);
    if (!image) return {};
    for (UINT y = 0; y < ThumbSize; ++y) {
        const auto* row = data + y * stride;
        for (UINT x = 0; x < ThumbSize; ++x) {
            for (UINT c = 0; c < 3; ++c) {
                (*image)[c * svdlut::InputPixels + y * ThumbSize + x] = row[4 * x + c] * (1.0f / 255.0f);
            }
        }
    }
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
    constexpr float normalized = 1.0f / (255 * (block / step) * (block / step));
    for (unsigned y = 0; y < ThumbSize; y += step) {
        const auto* row = data + (y + step / 2) * stride;
        for (unsigned x = 0; x < ThumbSize; x += step) {
            const auto tile = (y / block) * SignatureSize + x / block;
            for (unsigned c = 0; c < 3; ++c) {
                signature[tile * 3 + c] += row[(x + step / 2) * 4 + c] * normalized;
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
    auto& lut = m_svdLut;
    lut.model.Stop(fast || m_fastShutdown);
    lut.attempted = lut.failed = false;
    lut.clearViewSupported = false;
    lut.picture.Release();
    lut.nativeThumbnail.Release();
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
    desc.ByteWidth = 8 * sizeof(float);
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
    const auto hr = lut.thumbnail.CheckCreate(m_pDevice, DXGI_FORMAT_R8G8B8A8_UNORM,
        ThumbSize, ThumbSize, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr)) return hr;
    return lut.staging.CheckCreate(m_pDevice, DXGI_FORMAT_R8G8B8A8_UNORM,
        ThumbSize, ThumbSize, Tex2D_StagingRead);
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
        FAILED(TextureCopyRect(*source, lut.thumbnail.pTexture, sourceRect, thumbnail, m_pPS_Simple,
                               nullptr, 0, false, true))) return;
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
    return SvdLutGrade{shader, scene->view.Get(), fade * (float(lut.strength) / SVDLUT_STRENGTH_MAX)};
}

HRESULT CDX11VideoProcessor::ApplySvdLutGrade(const SvdLutGrade& grade, const Tex2D_t& source, ID3D11Texture2D* target,
    const CRect& area, const CRect& visible, CPoint sourceOffset)
{
    auto& lut = m_svdLut;
    const float constants[8] = {
        float(visible.left), float(visible.top), float(visible.Width() - 1), float(visible.Height() - 1),
        grade.blend, float(sourceOffset.x), float(sourceOffset.y), 0
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
    m_pDeviceContext->PSSetSamplers(1, 1, &noSampler);
    lut.failed = FAILED(hr);
    lut.active = !lut.failed;
    return hr;
}

HRESULT CDX11VideoProcessor::DrawCompletedSvdLut(const CompletedFrame& frame, ID3D11Texture2D* target,
    const CRect& picture, bool geometryChanging)
{
    if (m_pFilter->m_svdLutOff.load()) return S_FALSE;
    if (m_pFilter->m_svdLutRaw.load())
        return DrawCompletedSvdLutRaw(frame, target, picture, geometryChanging);
    const auto grade = PrepareSvdLutGrade(frame.pts);
    if (!grade) return S_FALSE;

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    const CRect area(0, 0, desc.Width, desc.Height);
    CRect visible;
    if (!SupportedSdrFormat(desc.Format) || !visible.IntersectRect(picture, area) ||
        visible.Width() < 2 || visible.Height() < 2) return S_FALSE;

    const auto& source = frame.image.desc;
    if (frame.picture == picture && source.Width == desc.Width && source.Height == desc.Height &&
        source.Format == desc.Format && frame.image.pShaderResource) {
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
    return ApplySvdLutGrade(*grade, pictureTexture, target, area, visible, CPoint(0, 0));
}

void CDX11VideoProcessor::DrawSvdLut(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp)
{
    if (m_pFilter->m_svdLutOff.load()) return;
    if (m_pFilter->m_svdLutRaw.load()) {
        DrawSvdLutRaw(target, picture, timestamp);
        return;
    }
    const auto grade = PrepareSvdLutGrade(timestamp);
    if (!grade || !target) return;

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
    ApplySvdLutGrade(*grade, lut.picture, target, visible, visible, CPoint(-visible.left, -visible.top));
}

#include "Edition/SvdLutRawMode.inc"
