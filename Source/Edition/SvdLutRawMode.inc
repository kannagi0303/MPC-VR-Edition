// SPDX-License-Identifier: GPL-3.0-or-later
// Separate experimental SVDLUT path B. Included by DX11VideoProcessorSvdLut.cpp.
// RAW/Temporal B use an independent 0..200 output-strength mix; B1 timing/LUT model stay unchanged.

void CDX11VideoProcessor::ReleaseSvdLutRawTest(bool fast)
{
    auto& b = m_svdLutRawTest;
    b.model.Stop(fast || m_fastShutdown);
    b.frames = {};
    for (auto& capture : b.captures) {
        capture.pending = false;
        capture.input.reset();
        capture.staging.Release();
    }
    b.inFlight = false;
    b.inFlightPts = INVALID_TIME;
    b.inFlightFence = b.inFlightStart = 0;
    b.frameSlot = b.captureSlot = 0;
    b.heldView.Reset();
    b.heldParameters.reset();
    b.b2CachedView.Reset();
    b.b2CachedSourcePts = INVALID_TIME;
    b.b2CachedRefinedReady = false;
    b.b2CachedTone = b.b2LastTone = svdlut::b2::luma::NoLuma;
    b.b2ShotPts = INVALID_TIME;
    b.b2ShotValid = false;
    b.b2ShotSignature = {};
    b.b2LastSourceShiftEv = b.b2LastResidualEv = 0.0f;
    b.b2LastSamples = 0;
    b.grayProbe = {};
    b.b2DrawRecords = {};
    b.b2DrawSlot = 0;
    b.b2Presented = {};
    b.b2Analyses = b.b2AdjustedDraws = 0;
    b.b2AnalysisTicks = b.b2BudgetSkips = b.b2ShotResets = 0;
    b.b2ShaderAttempted = b.b2ShaderFailed = false;
    b.b2MemoryShaderAttempted = b.b2MemoryShaderFailed = false;
    b.r1GamutShaderAttempted = b.r1GamutShaderFailed = false;
    b.m1ShaderAttempted = b.m1ShaderFailed = false;
    b.r1M1ShaderAttempted = b.r1M1ShaderFailed = false;
    b.m1SkinDraws = b.d1DepthDraws = b.d1FallbackDraws = 0;
    b.b2MemoryDraws = 0;
    b.r1GamutDraws = 0;
    b.heldPts = INVALID_TIME;
    b.heldSignatureReady = false;
    b.appliedDraws = b.missedResult = 0;
    b.presentedGraded = b.presentedUngraded = 0;
    b.skippedCapture = b.reusedGrade = b.ungraded = 0;
    b.requests = b.completed = b.modelTicks = b.stageTicks = 0;
    b.temporalFrames = b.temporalUsed = b.temporalRejected = b.temporalFuture = b.prefetched = b.earlyPrimed = 0;
    b.lastWindow = 0;
    b.attempted = b.failed = b.active = false;
    b.thumbnail.Release();
    b.staging.Release();
    b.picture.Release();
    b.sampler.Release();
    b.constants.Release();
    b.shader.Release();
    b.b2LuminanceShader.Release();
    b.b2MemoryShader.Release();
    b.r1GamutShader.Release();
    b.m1SkinShader.Release();
    b.r1M1SkinShader.Release();
}

bool CDX11VideoProcessor::StartSvdLutRawTest()
{
    auto& b = m_svdLutRawTest;
    b.attempted = true;
    CComQIPtr<IDXGIDevice> dxgi(m_pDevice.p);
    CComPtr<IDXGIAdapter> adapter;
    b.failed = !dxgi || FAILED(dxgi->GetAdapter(&adapter)) ||
        !b.model.Start(adapter, m_pDevice, NgxRuntime::EditionDir(), true);
    return !b.failed;
}

HRESULT CDX11VideoProcessor::PrepareSvdLutRawResources()
{
    auto& b = m_svdLutRawTest;
    HRESULT hr = b.thumbnail.CheckCreate(m_pDevice, ThumbFormat,
        ThumbSize, ThumbSize, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr)) return hr;
    if (!m_pFilter->m_svdLutTemporal.load()) {
        hr = b.staging.CheckCreate(m_pDevice, ThumbFormat,
            ThumbSize, ThumbSize, Tex2D_StagingRead);
        if (FAILED(hr)) return hr;
    }
    if (!b.constants) {
        D3D11_BUFFER_DESC cb{};
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        // First eight floats are B1-identical; the third float4 is read only
        // by the independently enabled B2 shader.
        cb.ByteWidth = 16 * sizeof(float);
        hr = m_pDevice->CreateBuffer(&cb, nullptr, &b.constants);
        if (FAILED(hr)) return hr;
    }
    if (!b.sampler) {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        hr = m_pDevice->CreateSamplerState(&desc, &b.sampler);
        if (FAILED(hr)) return hr;
    }
    if (!b.shader) {
        CComPtr<ID3DBlob> code;
        hr = CompileSvdLutOutputShader(SvdLutRawShader, nullptr, &code);
        if (FAILED(hr)) return hr;
        hr = m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &b.shader);
        if (FAILED(hr)) return hr;
    }
    // B2 shader compilation is a one-time opt-in setup operation, never a
    // per-pixel-draw retry. Failure silently retains the intact B1 shader,
    // but is visible in Edition details.
    if (m_pFilter->m_svdLutTemporal.load() && m_pFilter->m_svdLutB2Luminance.load() &&
        !b.b2ShaderAttempted) {
        b.b2ShaderAttempted = true;
        CComPtr<ID3DBlob> code;
        HRESULT compiled = CompileSvdLutOutputShader(SvdLutB2LuminanceShader, nullptr, &code);
        if (SUCCEEDED(compiled))
            compiled = m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                nullptr, &b.b2LuminanceShader);
        b.b2ShaderFailed = FAILED(compiled);
    }
    // M2: compile the selected M1 variant FIRST, so an M1-only test does
    // not also compile a base B2.2/R1 shader that will never be drawn.
    // On failure, the already-existing B2.2/R1 fallback is compiled below.
    // Both paths keep the same LUT and single drawing pass.
    if (m_pFilter->m_svdLutTemporal.load() && m_pFilter->m_svdLutM1Skin.load() &&
        (m_pFilter->m_svdLutB2Memory.load() || m_pFilter->m_svdLutR1Gamut.load())) {
        const bool r1 = m_pFilter->m_svdLutR1Gamut.load();
        bool& attempted = r1 ? b.r1M1ShaderAttempted : b.m1ShaderAttempted;
        bool& failed = r1 ? b.r1M1ShaderFailed : b.m1ShaderFailed;
        auto& shader = r1 ? b.r1M1SkinShader : b.m1SkinShader;
        if (!attempted) {
            attempted = true;
            static const D3D_SHADER_MACRO m1Defines[] = {
                {"SVDLUT_M1_SKIN_GUARD", "1"}, {nullptr, nullptr}
            };
            static const D3D_SHADER_MACRO r1m1Defines[] = {
                {"SVDLUT_R1_CONTINUOUS", "1"},
                {"SVDLUT_M1_SKIN_GUARD", "1"}, {nullptr, nullptr}
            };
            CComPtr<ID3DBlob> code;
            HRESULT compiled = CompileSvdLutOutputShader(SvdLutB2MemoryShader,
                r1 ? r1m1Defines : m1Defines, &code);
            if (SUCCEEDED(compiled))
                compiled = m_pDevice->CreatePixelShader(code->GetBufferPointer(),
                    code->GetBufferSize(), nullptr, &shader);
            failed = FAILED(compiled);
        }
    }
    // Avoid compiling BOTH base and guarded variants at first enable.
    // A disabled M1 compiles the original variant when needed; an M1 compile
    // failure still compiles the original variant on this same Prepare call.
    const bool needB2Memory = m_pFilter->m_svdLutTemporal.load() &&
        m_pFilter->m_svdLutB2Memory.load() && !m_pFilter->m_svdLutR1Gamut.load() &&
        (!m_pFilter->m_svdLutM1Skin.load() || b.m1ShaderFailed);
    if (needB2Memory && !b.b2MemoryShaderAttempted) {
        b.b2MemoryShaderAttempted = true;
        CComPtr<ID3DBlob> code;
        HRESULT compiled = CompileSvdLutOutputShader(SvdLutB2MemoryShader, nullptr, &code);
        if (SUCCEEDED(compiled))
            compiled = m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                nullptr, &b.b2MemoryShader);
        b.b2MemoryShaderFailed = FAILED(compiled);
    }
    // Opt-in R1 compiles only once. The same HLSL, constants, LUT and draw
    // path are used; a shader macro replaces just the gamut continuity rules.
    const bool needR1Gamut = m_pFilter->m_svdLutTemporal.load() &&
        m_pFilter->m_svdLutR1Gamut.load() &&
        (!m_pFilter->m_svdLutM1Skin.load() || b.r1M1ShaderFailed);
    if (needR1Gamut && !b.r1GamutShaderAttempted) {
        b.r1GamutShaderAttempted = true;
        static const D3D_SHADER_MACRO r1Defines[] = {
            {"SVDLUT_R1_CONTINUOUS", "1"}, {nullptr, nullptr}
        };
        CComPtr<ID3DBlob> code;
        HRESULT compiled = CompileSvdLutOutputShader(SvdLutB2MemoryShader, r1Defines, &code);
        if (SUCCEEDED(compiled))
            compiled = m_pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                nullptr, &b.r1GamutShader);
        b.r1GamutShaderFailed = FAILED(compiled);
    }
    return S_OK;
}

HRESULT CDX11VideoProcessor::EnsureSvdLutRawPicture(DXGI_FORMAT format, UINT width, UINT height)
{
    auto& target = m_svdLutRawTest.picture;
    const bool same = target.pTexture && target.desc.Format == format;
    if (same && target.desc.Width >= width && target.desc.Height >= height) return S_OK;
    const auto& screen = m_pFilter->m_DisplayConfig;
    const UINT w = (std::max)({width, UINT(screen.width), same ? target.desc.Width : 0u});
    const UINT h = (std::max)({height, UINT(screen.height), same ? target.desc.Height : 0u});
    Tex2D_t replacement;
    HRESULT hr = replacement.Create(m_pDevice, format, w, h, Tex2D_DefaultShaderRTarget);
    if (FAILED(hr) && (w != width || h != height))
        hr = replacement.Create(m_pDevice, format, width, height, Tex2D_DefaultShaderRTarget);
    if (SUCCEEDED(hr)) target = std::move(replacement);
    return hr;
}

// Nonblocking Temporal B only. The screenshot is reduced to 256x256 on the
// immediate D3D11 context, copied into a small STAGING RING, then polled with
// DO_NOT_WAIT. Never map, wait for a model, or upload a view while presenting
// an original frame unless the data is already ready.
void CDX11VideoProcessor::ResetSvdLutTemporalQueue()
{
    auto& b = m_svdLutRawTest;
    b.model.Invalidate();
    for (auto& capture : b.captures) {
        capture.pending = false;
        capture.input.reset();
        capture.pts = INVALID_TIME;
        capture.fence = 0;
    }
    b.inFlight = false;
    b.inFlightPts = INVALID_TIME;
    b.inFlightFence = b.inFlightStart = 0;
    b.captureSlot = b.frameSlot = 0;
    b.frames = {};
    b.heldView.Reset();
    b.heldParameters.reset();
    b.b2CachedView.Reset();
    b.b2CachedSourcePts = INVALID_TIME;
    b.b2CachedRefinedReady = false;
    b.b2CachedTone = b.b2LastTone = svdlut::b2::luma::NoLuma;
    b.b2ShotPts = INVALID_TIME;
    b.b2ShotValid = false;
    b.b2ShotSignature = {};
    b.b2LastSourceShiftEv = b.b2LastResidualEv = 0.0f;
    b.b2LastSamples = 0;
    b.grayProbe = {};
    b.b2DrawRecords = {};
    b.b2DrawSlot = 0;
    b.b2Presented = {};
    b.heldPts = INVALID_TIME;
    b.heldSignatureReady = false;
    b.lastWindow = 0;
}

void CDX11VideoProcessor::PollSvdLutTemporalQueue()
{
    auto& b = m_svdLutRawTest;
    if (b.model.GetState() == svdlut::ModelWorker::State::Failed) {
        b.failed = true;
        return;
    }
    if (b.inFlight) {
        auto result = b.model.TryTakeFrameResult();
        if (result.view && result.parameters) {
            const auto finished = GetPreciseTick();
            auto entry = std::find_if(b.frames.begin(), b.frames.end(), [&](const auto& frame) {
                return frame.pts == b.inFlightPts && frame.fence == b.inFlightFence;
            });
            svdlut::temporal::Features features{};
            constexpr size_t Values = sizeof(svdlut::Parameters::lut) / sizeof(float);
            const auto packed = std::span<const float>(&result.parameters->lut[0][0], Values);
            if (svdlut::temporal::BuildColorFeatures(packed, features)) {
                // Preserve a completed result even if the corresponding frame
                // was evicted while the GPU worker was running.
                if (b.heldPts == INVALID_TIME || b.inFlightPts >= b.heldPts) {
                    b.heldView = result.view;
                    b.heldParameters = result.parameters;
                    b.heldPts = b.inFlightPts;
                    b.heldSignature = b.inFlightSignature;
                    b.heldSignatureReady = true;
                }
                if (entry != b.frames.end()) {
                    entry->view = std::move(result.view);
                    entry->parameters = std::move(result.parameters);
                    entry->colorFeatures = features;
                } else {
                    ++b.missedResult;
                }
                ++b.completed;
                b.modelTicks += finished - b.inFlightStart;
            }
            b.inFlight = false;
        }
    }
    // GPU readback is independent of inference. In the previous pipeline
    // thumbnails were not mapped at all while DirectML was busy, so even a
    // completed original-frame signature was marked unknown. The present path
    // then alternated between an ungraded frame and a graded one. Always
    // harvest ready ORIGINAL thumbnails first (DO_NOT_WAIT), with no extra
    // inference or full-resolution passes.
    for (auto& capture : b.captures) {
        if (!capture.pending) continue;
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT hr = m_pDeviceContext->Map(capture.staging.pTexture, 0,
            D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        capture.pending = false;
        if (FAILED(hr)) {
            capture.input.reset();
            capture.pts = INVALID_TIME;
            ++b.skippedCapture;
            continue;
        }
        const auto frame = std::find_if(b.frames.begin(), b.frames.end(), [&](const auto& f) {
            return f.pts == capture.pts && f.fence == capture.fence;
        });
        if (frame != b.frames.end()) {
            frame->signature = ReadSignature(static_cast<const uint8_t*>(map.pData), map.RowPitch);
            frame->signatureReady = true;
            if (m_pFilter->m_svdLutB2Luminance.load()) {
                frame->b2Samples = svdlut::b2::refined::ReadSamples(
                    static_cast<const uint8_t*>(map.pData), map.RowPitch);
                frame->b2SamplesReady = true;
            }
            capture.input = ReadSvdLutImage(static_cast<const uint8_t*>(map.pData), map.RowPitch);
        }
        m_pDeviceContext->Unmap(capture.staging.pTexture, 0);
        if (frame == b.frames.end() || !capture.input) {
            capture.input.reset();
            capture.pts = INVALID_TIME;
            ++b.skippedCapture;
        }
    }

    if (b.inFlight || !b.model.ReadyForInput()) return;
    // Submit the oldest ready ORIGINAL thumbnail. Up to three CPU thumbnails
    // can wait while the one worker is busy; never dispatch a generated frame.
    SvdLutTemporalCapture* oldest = nullptr;
    for (auto& capture : b.captures) {
        if (!capture.input) continue;
        const auto frame = std::find_if(b.frames.begin(), b.frames.end(), [&](const auto& f) {
            return f.pts == capture.pts && f.fence == capture.fence;
        });
        if (frame == b.frames.end()) {
            capture.input.reset();
            capture.pts = INVALID_TIME;
            ++b.skippedCapture;
            continue;
        }
        if (!oldest || capture.pts < oldest->pts) oldest = &capture;
    }
    if (!oldest) return;
    const auto frame = std::find_if(b.frames.begin(), b.frames.end(), [&](const auto& f) {
        return f.pts == oldest->pts && f.fence == oldest->fence;
    });
    if (frame == b.frames.end()) return;
    const auto pts = oldest->pts;
    const auto fence = oldest->fence;
    const auto signature = frame->signature;
    if (!b.model.Submit(std::move(oldest->input))) {
        ++b.skippedCapture;
        oldest->input.reset();
        oldest->pts = INVALID_TIME;
        return;
    }
    oldest->pts = INVALID_TIME;
    b.inFlight = true;
    b.inFlightPts = pts;
    b.inFlightFence = fence;
    b.inFlightSignature = signature;
    b.inFlightStart = GetPreciseTick();
    ++b.requests;
}

CDX11VideoProcessor::SvdLutRawFrame* CDX11VideoProcessor::QueueSvdLutTemporalFrame(
    const Tex2D_t& source, const CRect& sample, REFERENCE_TIME pts, uint64_t fence)
{
    auto& b = m_svdLutRawTest;
    if (pts == INVALID_TIME) return nullptr;
    // A redraw, retry, or future prefetch must never cause a second inference.
    for (auto& entry : b.frames)
        if (entry.pts == pts && entry.fence == fence) return &entry;
    // Admission control protects the Completed Video Buffer. Under pressure,
    // skip optional *LUT inference*, never a decoded/original video frame.
    // Allow an idle worker to recover even if the buffer remains shallow.
    if (b.completed && m_completingSample &&
        (b.inFlight || std::any_of(b.captures.begin(), b.captures.end(),
            [](const auto& c) { return c.pending || bool(c.input); }))) {
        CRefTime stream;
        if (SUCCEEDED(m_pFilter->StreamTime(stream)) &&
            GetCompletedBufferTime(stream).ready < 80 * 10000) {
            ++b.skippedCapture;
            return nullptr;
        }
    }
    SvdLutTemporalCapture* slot = nullptr;
    for (size_t i = 0; i < b.captures.size(); ++i) {
        auto& candidate = b.captures[(b.captureSlot + i) % b.captures.size()];
        if (!candidate.pending && !candidate.input) { slot = &candidate; b.captureSlot = (b.captureSlot + i + 1) % b.captures.size(); break; }
    }
    if (!slot) {
        // Playback is deadline-ordered: preserve the oldest (nearest-to-
        // presentation) outstanding thumbnail. Evict only a LATER pending
        // thumbnail if an earlier source frame arrives out of order.
        // Previous code evicted the oldest every time the small ring filled,
        // repeatedly starving the startup/seek queue head of its signature.
        auto* farthest = &*std::max_element(b.captures.begin(), b.captures.end(),
            [](const auto& x, const auto& y) { return x.pts < y.pts; });
        if (pts >= farthest->pts) {
            ++b.skippedCapture;
            return nullptr;
        }
        slot = farthest;
        for (auto& old : b.frames) {
            if (old.pts == slot->pts && old.fence == slot->fence && !old.view) {
                old = {};
                break;
            }
        }
        slot->pending = false;
        slot->input.reset();
        slot->pts = INVALID_TIME;
        ++b.skippedCapture;
    }
    if (FAILED(slot->staging.CheckCreate(m_pDevice, ThumbFormat,
            ThumbSize, ThumbSize, Tex2D_StagingRead))) { ++b.skippedCapture; return nullptr; }
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    const HRESULT hr = RenderSvdLutThumbnail(source, b.thumbnail.pTexture, sample);
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    if (FAILED(hr)) { ++b.skippedCapture; return nullptr; }
    m_pDeviceContext->CopyResource(slot->staging.pTexture, b.thumbnail.pTexture);
    slot->pts = pts;
    slot->fence = fence;
    slot->input.reset();
    slot->pending = true;
    auto& entry = b.frames[b.frameSlot];
    b.frameSlot = (b.frameSlot + 1) % b.frames.size();
    entry = {};
    entry.pts = pts;
    entry.fence = fence;
    return &entry;
}

namespace {
// The only scene evidence used by Temporal B comes from decoded/original
// completed-frame thumbnails. Never inspect MFG-generated frames. A missing
// signature for a queued original means "unknown", NOT "same scene".
template <typename Frames>
const auto* TemporalReferenceFrame(const Frames& frames, REFERENCE_TIME pts, uint64_t fence)
{
    using Frame = typename Frames::value_type;
    const Frame* exact = nullptr;
    const Frame* preceding = nullptr;
    const Frame* following = nullptr;
    const Frame* newestOriginal = nullptr;
    for (const auto& frame : frames) {
        if (frame.pts == INVALID_TIME) continue;
        if (frame.pts == pts && (fence == 0 || frame.fence == fence)) exact = &frame;
        if (frame.pts <= pts && (!newestOriginal || newestOriginal->pts < frame.pts))
            newestOriginal = &frame;
        if (frame.pts <= pts && frame.signatureReady &&
            (!preceding || preceding->pts < frame.pts)) preceding = &frame;
        if (frame.pts > pts && frame.signatureReady &&
            (!following || following->pts > frame.pts)) following = &frame;
    }
    if (exact) return exact->signatureReady ? exact : nullptr;
    if (newestOriginal && !newestOriginal->signatureReady) return static_cast<const Frame*>(nullptr);
    // A generated frame belongs between ORIGINALs. Prefer the original
    // before its timestamp, and only borrow the next one's signature when
    // no earlier original was captured near this timestamp.
    if (preceding && pts - preceding->pts <= 80 * 10000) return preceding;
    if (following && following->pts - pts <= 50 * 10000) return following;
    return static_cast<const Frame*>(nullptr);
}

template <typename Frames>
bool TemporalSceneCompatible(const Frames& frames, REFERENCE_TIME fromPts, const float* fromSignature,
    REFERENCE_TIME toPts, const float* toSignature)
{
    std::array<svdlut::temporal::SceneProbe, 16> probes{};
    size_t count = 0;
    for (const auto& frame : frames) {
        if (frame.signatureReady && frame.pts != INVALID_TIME)
            probes[count++] = {frame.signature.data(), frame.pts};
    }
    return svdlut::temporal::SameOriginalScene(std::span(probes.data(), count),
        {fromSignature, fromPts}, {toSignature, toPts});
}
} // namespace

ID3D11ShaderResourceView* CDX11VideoProcessor::SvdLutTemporalReadyView(REFERENCE_TIME pts, uint64_t fence)
{
    auto& b = m_svdLutRawTest;
    SvdLutRawFrame* current = nullptr;
    for (auto& frame : b.frames)
        if (frame.pts == pts && frame.fence == fence) { current = &frame; break; }

    // The current original's readback may still be pending. NEVER substitute
    // the previous shot's color grading before inspecting its thumbnail.
    const auto* reference = TemporalReferenceFrame(b.frames, pts, fence);
    const auto compatible = [&](REFERENCE_TIME otherPts, const float* otherSignature) {
        return reference && reference->signatureReady && otherSignature &&
            TemporalSceneCompatible(b.frames, otherPts, otherSignature,
                reference->pts, reference->signature.data());
    };
    const auto remember = [&](SvdLutRawFrame& frame) -> ID3D11ShaderResourceView* {
        auto* view = SvdLutRawViewForFrame(frame);
        if (view && frame.signatureReady && (b.heldPts == INVALID_TIME || frame.pts >= b.heldPts)) {
            b.heldView = view;
            b.heldParameters = frame.temporalView && view == frame.temporalView.Get() ?
                frame.temporalParameters : frame.parameters;
            b.heldPts = frame.pts;
            b.heldSignature = frame.signature;
            b.heldSignatureReady = true;
        }
        return view;
    };
    if (current && current->view) return remember(*current);

    // No original-frame evidence: display the unmodified source briefly,
    // rather than leaking the previous shot's LUT across a cut.
    if (!reference) { ++b.ungraded; return nullptr; }

    SvdLutRawFrame* previous = nullptr;
    for (auto& frame : b.frames) {
        if (!frame.view || !frame.signatureReady || frame.pts == INVALID_TIME || frame.pts > pts ||
            pts - frame.pts > svdlut::temporal::MaximumDistancePts ||
            !compatible(frame.pts, frame.signature.data())) continue;
        if (!previous || frame.pts > previous->pts) previous = &frame;
    }
    if (previous) {
        ++b.reusedGrade;
        return remember(*previous);
    }
    if (b.heldView && b.heldSignatureReady && b.heldPts != INVALID_TIME &&
        pts >= b.heldPts && pts - b.heldPts <= svdlut::temporal::MaximumDistancePts &&
        compatible(b.heldPts, b.heldSignature.data())) {
        ++b.reusedGrade;
        return b.heldView.Get();
    }
    ++b.ungraded;
    return nullptr;
}

CDX11VideoProcessor::SvdLutRawFrame* CDX11VideoProcessor::EvaluateSvdLutRawFrame(const Tex2D_t& source,
    const CRect& sample, REFERENCE_TIME pts, uint64_t fence)
{
    auto& b = m_svdLutRawTest;
    const bool temporal = m_pFilter->m_svdLutTemporal.load();
    // Never count a pause redraw or Present retry as a new model inference.
    for (auto& entry : b.frames)
        if (entry.pts == pts && entry.fence == fence && entry.view) return &entry;

    auto& entry = b.frames[b.frameSlot];
    b.frameSlot = (b.frameSlot + 1) % b.frames.size();
    entry = {};
    entry.pts = pts;
    entry.fence = fence;
    ++b.requests;
    const auto started = GetPreciseTick();
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    if (FAILED(RenderSvdLutThumbnail(source, b.thumbnail.pTexture, sample))) return nullptr;
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_pDeviceContext->CopyResource(b.staging.pTexture, b.thumbnail.pTexture);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_pDeviceContext->Map(b.staging.pTexture, 0, D3D11_MAP_READ, 0, &map))) return nullptr;
    // Statistical content signature is for detecting genuine illumination/
    // shot discontinuities, independent of scene LUT selection in mode A.
    if (temporal) {
        entry.signature = ReadSignature(static_cast<const uint8_t*>(map.pData), map.RowPitch);
        if (m_pFilter->m_svdLutB2Luminance.load()) {
            entry.b2Samples = svdlut::b2::refined::ReadSamples(
                static_cast<const uint8_t*>(map.pData), map.RowPitch);
            entry.b2SamplesReady = true;
        }
    }
    auto input = ReadSvdLutImage(static_cast<const uint8_t*>(map.pData), map.RowPitch);
    m_pDeviceContext->Unmap(b.staging.pTexture, 0);
    if (!input) return nullptr;

    const auto infer = GetPreciseTick();
    if (temporal) {
        auto result = b.model.EvaluateFrameWithParameters(std::move(input));
        entry.view = std::move(result.view);
        entry.parameters = std::move(result.parameters);
    } else {
        entry.view = b.model.EvaluateFrame(std::move(input));
    }
    if (!entry.view) return nullptr;
    if (temporal && entry.parameters) {
        constexpr size_t Values = sizeof(svdlut::Parameters::lut) / sizeof(float);
        const auto packed = std::span<const float>(&entry.parameters->lut[0][0], Values);
        if (!svdlut::temporal::BuildColorFeatures(packed, entry.colorFeatures)) return nullptr;
    }
    const auto end = GetPreciseTick();
    ++b.completed;
    b.modelTicks += end - infer;
    b.stageTicks += end - started;
    return &entry;
}

ID3D11ShaderResourceView* CDX11VideoProcessor::SvdLutRawViewForFrame(SvdLutRawFrame& current)
{
    auto& b = m_svdLutRawTest;
    if (!current.view || !m_pFilter->m_svdLutTemporal.load() || !current.parameters)
        return current.view.Get();
    if (current.temporalFinalized) return current.temporalView ? current.temporalView.Get() : current.view.Get();
    // Only freeze a *resolved* transform. A one-sample RAW fallback must not
    // permanently discard later ready neighborhood transforms on redraw.

    std::array<SvdLutRawFrame*, 16> candidates{};
    size_t count = 0;
    for (auto& frame : b.frames) {
        if (!frame.view || !frame.parameters || frame.pts == INVALID_TIME) continue;
        if (frame.pts < current.pts - svdlut::temporal::MaximumDistancePts ||
            frame.pts > current.pts + svdlut::temporal::MaximumDistancePts) continue;
        if (!frame.signatureReady || !current.signatureReady ||
            !TemporalSceneCompatible(b.frames, frame.pts, frame.signature.data(),
                current.pts, current.signature.data())) continue;
        if (count < candidates.size()) candidates[count++] = &frame;
    }
    std::sort(candidates.begin(), candidates.begin() + count, [](const auto* a, const auto* c) {
        return a->pts != c->pts ? a->pts < c->pts : a->fence < c->fence;
    });
    // Restrict to five closest real results on each side, even if the cache
    // contains more. Never fabricate future transforms from old values.
    const auto at = std::find(candidates.begin(), candidates.begin() + count, &current);
    if (at == candidates.begin() + count) return current.view.Get();
    const size_t center = static_cast<size_t>(at - candidates.begin());
    const size_t first = center > svdlut::temporal::NeighborsEachSide ?
        center - svdlut::temporal::NeighborsEachSide : 0;
    const size_t last = (std::min)(count, center + svdlut::temporal::NeighborsEachSide + 1);
    std::array<svdlut::temporal::Sample, svdlut::temporal::MaxWindow> ordered{};
    const size_t samples = last - first;
    for (size_t i = 0; i < samples; ++i) {
        const auto* f = candidates[first + i];
        ordered[i] = {&f->parameters->lut[0][0], f->signature.data(), f->pts, f->colorFeatures.data()};
    }
    // Blend() cannot synthesize a new LUT from only the current sample.
    // This is common during startup, after a cut, and on a repeated pre-draw.
    // Avoid allocating and zeroing a 204-KiB Parameters object on the
    // high-priority presenter just to take Blend()'s RAW/fallback path.
    svdlut::temporal::Summary result{};
    std::shared_ptr<svdlut::Parameters> blended;
    if (samples == 1) {
        result.used = 1; // Same documented result as Blend(one valid sample).
    } else {
        blended = std::make_shared<svdlut::Parameters>();
        constexpr size_t Values = sizeof(svdlut::Parameters::lut) / sizeof(float);
        auto output = std::span<float>(&blended->lut[0][0], Values);
        result = svdlut::temporal::Blend(std::span(ordered.data(), samples), center - first, output);
    }
    b.lastWindow = result.used;
    b.temporalUsed += result.used;
    b.temporalRejected += result.rejected;
    b.temporalFuture += result.future;
    if (!result.blended) {
        // With only 1-2 LUTs, the RAW estimator can spike even in a nearly
        // identical picture. Prefer the last trustworthy completed transform
        // over an unfiltered current outlier. Do NOT cross a known shot cut.
        SvdLutRawFrame* previous = nullptr;
        for (auto& frame : b.frames) {
            if (&frame == &current || !frame.view || !frame.signatureReady ||
                !current.signatureReady || frame.pts >= current.pts ||
                current.pts - frame.pts > svdlut::temporal::MaximumDistancePts) continue;
            const svdlut::temporal::Sample a{nullptr, frame.signature.data(), frame.pts, nullptr};
            const svdlut::temporal::Sample c{nullptr, current.signature.data(), current.pts, nullptr};
            if (!TemporalSceneCompatible(b.frames, frame.pts, frame.signature.data(),
                    current.pts, current.signature.data()) ||
                svdlut::temporal::PictureDistance(a, c) > .035f) continue;
            if (!previous || frame.pts > previous->pts) previous = &frame;
        }
        if (previous) {
            current.temporalView = previous->temporalView ? previous->temporalView : previous->view;
            current.temporalParameters = previous->temporalView ?
                previous->temporalParameters : previous->parameters;
            current.temporalFinalized = true;
            return current.temporalView.Get();
        }
        // First result or genuine lighting/cut: use unmodified RAW B.
        return current.view.Get();
    }
    current.temporalView = svdlut::CreateParametersView(m_pDevice, *blended);
    if (current.temporalView) {
        current.temporalParameters = std::move(blended); // B2 metadata, no effect on B1 math
        current.temporalFinalized = true;
        ++b.temporalFrames;
    }
    return current.temporalView ? current.temporalView.Get() : current.view.Get();
}

// Prime the first decoded/ORIGINAL frames while they are still in the
// Completed Video Buffer, before the normal pre-draw/present deadline.
// Starting the async model only from DrawCompletedSvdLutRaw() left roughly
// one display interval for thumbnail readback + DirectML inference, so the
// early seek/start frames alternated between graded and ungraded output.
// This work uses the same unique-frame capture queue and the existing worker:
// it changes WHEN the 256x256 source thumbnail is captured, not HOW MANY
// times an original frame is evaluated. Never analyze generated MFG frames.
void CDX11VideoProcessor::PrimeSvdLutTemporalFrames()
{
    auto& b = m_svdLutRawTest;
    if (!m_pFilter->m_svdLutRaw.load() || !m_pFilter->m_svdLutTemporal.load() ||
        m_pFilter->m_svdLutOff.load() || m_pFilter->m_svdLutBStrength.load() == 0 ||
        b.failed || m_completedFrames.empty() ||
        !m_completedFence || !m_pDeviceContext ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return;
    if (!b.attempted && !StartSvdLutRawTest()) return;
    if (b.model.GetState() != svdlut::ModelWorker::State::Ready) return;
    if (FAILED(PrepareSvdLutRawResources())) { b.failed = true; return; }

    PollSvdLutTemporalQueue();
    const uint64_t completed = m_completedFence->GetCompletedValue();
    if (completed == UINT64_MAX) return;

    // A bounded 125 ms source-media lookahead is enough to initiate a
    // 17-55 ms model dispatch well before display, without buffering the
    // whole 1-2 second Completed queue or outrunning the 16-result ring.
    constexpr REFERENCE_TIME lookahead = 125 * 10'000;
    const REFERENCE_TIME headPts = m_completedFrames.front().pts;
    size_t pending = std::count_if(b.captures.begin(), b.captures.end(),
        [](const auto& c) { return c.pending || bool(c.input); });
    for (const auto& frame : m_completedFrames) {
        if (pending >= 2) break;
        if (frame.phase != mfg::MaxGenerated || frame.pts == INVALID_TIME ||
            frame.pts < headPts || frame.pts - headPts > lookahead ||
            frame.fence > completed || !frame.image.pShaderResource) continue;
        if (std::any_of(b.frames.begin(), b.frames.end(), [&](const auto& f) {
                return f.pts == frame.pts && f.fence == frame.fence;
            })) continue;
        CRect sample;
        if (!sample.IntersectRect(frame.picture, CRect(0, 0,
                frame.image.desc.Width, frame.image.desc.Height)) ||
            sample.Width() < 2 || sample.Height() < 2) continue;
        if (QueueSvdLutTemporalFrame(frame.image, sample, frame.pts, frame.fence)) {
            ++b.prefetched;
            ++b.earlyPrimed;
            ++pending;
        }
    }
    // If there was no new thumbnail but a previous one finished, dispatch
    // its ready CPU data now. Never wait for the GPU or the model here.
    PollSvdLutTemporalQueue();
}

// Only borrow READY completed future frames. No enqueue/wait for nonexistent
// future video, no change to original-frame presentation or pacing.
void CDX11VideoProcessor::PrefetchSvdLutRawFuture(const CompletedFrame& current,
    bool geometryChanging)
{
    auto& b = m_svdLutRawTest;
    if (!m_pFilter->m_svdLutTemporal.load() || !m_completedFence ||
        m_completedFrames.empty() || geometryChanging) return;
    CRefTime stream;
    if (FAILED(m_pFilter->StreamTime(stream)) ||
        GetCompletedBufferTime(stream).ready < 250 * 10000) return;
    // During low buffer, spend GPU time on original frames, not speculation.
    // Build the upcoming original-frame signature even while the DirectML
    // worker is occupied. Background readback and inference use distinct slots.
    // Keep the outstanding thumbnails bounded; avoid replacing a useful
    // near-future capture with one farther ahead.
    if (std::count_if(b.captures.begin(), b.captures.end(), [](const auto& c) {
            return c.pending || bool(c.input);
        }) >= 2) return;
    const uint64_t finished = m_completedFence->GetCompletedValue();
    if (finished == UINT64_MAX) return;
    // At most one speculative evaluation per displayed frame. Steady-state
    // cost remains one inference per NEW frame (the next frame is reused).
    for (const auto& future : m_completedFrames) {
        if (future.phase != mfg::MaxGenerated || future.pts <= current.pts ||
            future.pts > current.pts + svdlut::temporal::MaximumDistancePts ||
            future.fence > finished ||
            future.picture != current.picture || future.projected != current.projected) continue;
        if (std::any_of(b.frames.begin(), b.frames.end(), [&](const auto& f) {
                return f.pts == future.pts && f.fence == future.fence;
            })) continue;
        // Sample directly from the *completed source-size image*. Never
        // render a speculative future into a full-display-size intermediate.
        if (!future.image.pShaderResource) continue;
        CRect sample;
        if (!sample.IntersectRect(future.picture, CRect(0, 0,
                future.image.desc.Width, future.image.desc.Height)) ||
            sample.Width() < 2 || sample.Height() < 2) continue;
        if (QueueSvdLutTemporalFrame(future.image, sample, future.pts, future.fence)) ++b.prefetched;
        break;
    }
}

// B2.1 source-adaptive code-luma participation. B1's selected
// view and temporal LUT are not altered; no additional GPU readback/inference.
// Calibration happens at most once for each (source PTS, selected LUT view).
// MFG-generated frames reuse the nearest ORIGINAL frame's source samples.
std::array<float, 3> CDX11VideoProcessor::SvdLutB2ToneForView(REFERENCE_TIME pts,
    uint64_t fence, ID3D11ShaderResourceView* selected)
{
    auto& b = m_svdLutRawTest;
    constexpr auto identity = svdlut::b2::luma::NoLuma; // safe if B2.1 enabled, evidence unavailable
    if (!selected) return identity;
    if (m_pFilter->m_filterState == State_Paused &&
        b.b2Presented.pts == pts && b.b2Presented.view.Get() == selected &&
        b.b2Presented.tone[0] <= svdlut::b2::luma::MaximumParticipation)
        return b.b2Presented.tone;
    const auto* reference = TemporalReferenceFrame(b.frames, pts, fence);
    if (!reference || !reference->signatureReady) return identity;
    if (b.b2CachedView.Get() == selected && b.b2CachedSourcePts == reference->pts &&
        (b.b2CachedRefinedReady || !reference->b2SamplesReady))
        return b.b2CachedTone;

    // B2 follows B1's *original-thumbnail* scene compatibility test. A cut
    // immediately removes the old scene anchor; a moving/lighting-changing
    // shot is allowed to adapt at a measured rate, without chasing each tile.
    const bool withinWindow = b.b2ShotValid &&
        (std::max)(reference->pts, b.b2ShotPts) -
            (std::min)(reference->pts, b.b2ShotPts) <=
                svdlut::temporal::MaximumDistancePts;
    // The very same ORIGINAL PTS can be redrawn with a newer B1 temporal
    // view after future model results arrive. Treat that as SAME shot,
    // rather than reapplying a raw, unanchored B2 estimate.
    const bool sameShot = withinWindow &&
        (reference->pts == b.b2ShotPts || TemporalSceneCompatible(b.frames,
            b.b2ShotPts, b.b2ShotSignature.data(), reference->pts,
            reference->signature.data()));

    // Hard protection of the Completed Video Buffer: if measured B2 CPU work
    // ever becomes expensive and READY buffer is shallow, defer the optional
    // re-calibration. Do NOT skip B1, DirectML or any original video frame.
    // Reuse exact cached LUT/source matches. Under pressure, a new untested
    // candidate uses zero luma participation; a new shot is evaluated normally.
    bool pressure = false;
    if (sameShot && b.b2Analyses >= 8 && m_completingSample) {
        const auto ticks = GetPreciseTicksPerSecond();
        if (ticks && double(b.b2AnalysisTicks) * 1000.0 / double(ticks) /
                double(b.b2Analyses) > .60) {
            CRefTime stream;
            pressure = SUCCEEDED(m_pFilter->StreamTime(stream)) &&
                GetCompletedBufferTime(stream).ready < 200 * 10000;
        }
    }

    svdlut::b2::Tone tone{identity};
    bool measured = false;
    if (pressure) {
        // Changed LUT not measured under pressure: conservative zero luma.
        ++b.b2BudgetSkips;
    } else {
        std::shared_ptr<const svdlut::Parameters> chosen;
        const auto match = [&](const SvdLutRawFrame& frame) {
            if (frame.temporalView.Get() == selected && frame.temporalParameters)
                return frame.temporalParameters;
            if (frame.view.Get() == selected && frame.parameters)
                return frame.parameters;
            return std::shared_ptr<const svdlut::Parameters>{};
        };
        for (const auto& frame : b.frames) {
            chosen = match(frame);
            if (chosen) break;
        }
        if (!chosen && b.heldView.Get() == selected) chosen = b.heldParameters;
        if (chosen) {
            const uint64_t started = GetPreciseTick();
            constexpr size_t Count = sizeof(svdlut::Parameters::lut) / sizeof(float);
            const auto result = svdlut::b2::luma::Analyze(
                reference->b2SamplesReady ?
                    std::span<const float>(reference->b2Samples.data(), reference->b2Samples.size()) :
                    std::span<const float>{},
                std::span<const float>(&chosen->lut[0][0], Count));
            b.b2AnalysisTicks += GetPreciseTick() - started;
            ++b.b2Analyses;
            b.b2LastSamples = result.samples;
            b.b2LastSourceShiftEv = result.sourceShiftEv;
            b.b2LastResidualEv = result.residualEv;
            tone = result.tone;
            measured = result.samples >= 8;
            if (measured && sameShot) {
                svdlut::b2::Tone old{};
                old.gains = b.b2LastTone;
                const float release = reference->pts > b.b2ShotPts ?
                    1.0f - std::exp(-float(reference->pts - b.b2ShotPts) / 1'800'000.0f) : 0.0f;
                tone = svdlut::b2::luma::ShotUpdate(tone, old, release);
            }
        }
    }
    // Only a valid, new ORIGINAL observation advances the shot anchor;
    // synthetic MFG frames and repeated/pre-draw invocations do not.
    if (measured && (!b.b2ShotValid || reference->pts > b.b2ShotPts)) {
        if (b.b2ShotValid && !sameShot) ++b.b2ShotResets;
        b.b2ShotPts = reference->pts;
        b.b2ShotSignature = reference->signature;
        b.b2ShotValid = true;
        b.b2LastTone = tone.gains;
    } else if (measured && reference->pts == b.b2ShotPts && sameShot) {
        // A revised B1 LUT for the same source frame must remain coherent.
        b.b2LastTone = tone.gains;
    }
    b.b2CachedView = selected;
    b.b2CachedSourcePts = reference->pts;
    b.b2CachedRefinedReady = reference->b2SamplesReady;
    b.b2CachedTone = tone.gains;
    return tone.gains;
}

void CDX11VideoProcessor::RememberPresentedSvdLutB2(REFERENCE_TIME pts, uint64_t fence)
{
    auto& b = m_svdLutRawTest;
    b.b2Presented = {};
    if (!m_pFilter->m_svdLutRaw.load() || !m_pFilter->m_svdLutTemporal.load() ||
        m_pFilter->m_svdLutOff.load() ||
        (!m_pFilter->m_svdLutB2Luminance.load() && !m_pFilter->m_svdLutB2Memory.load() &&
         !m_pFilter->m_svdLutR1Gamut.load() && !PerceptualEnabled()) ||
        !b.active) return;
    // The cached pre-draw may be presented without an additional draw.
    // Match both original PTS and original fence before Present changes it.
    for (const auto& record : b.b2DrawRecords) {
        if (record.pts == pts && record.fence == fence && record.view) {
            b.b2Presented = record;
            break;
        }
    }
}

HRESULT CDX11VideoProcessor::ApplySvdLutRawFrame(const Tex2D_t& source, ID3D11Texture2D* target,
    const CRect& area, const CRect& visible, CPoint offset, REFERENCE_TIME pts, uint64_t fence,
    bool allowTemporalCapture, bool allowDepth)
{
    auto& b = m_svdLutRawTest;
    CRect sampled(visible);
    sampled.OffsetRect(offset);
    ID3D11ShaderResourceView* lut = nullptr;
    const bool b2LumEnabled = m_pFilter->m_svdLutTemporal.load() &&
        m_pFilter->m_svdLutB2Luminance.load();
    const bool b2MemoryEnabled = m_pFilter->m_svdLutTemporal.load() &&
        m_pFilter->m_svdLutB2Memory.load();
    const bool r1GamutEnabled = m_pFilter->m_svdLutTemporal.load() &&
        m_pFilter->m_svdLutR1Gamut.load();
    const bool m1SkinEnabled = m_pFilter->m_svdLutTemporal.load() &&
        m_pFilter->m_svdLutM1Skin.load() && (b2MemoryEnabled || r1GamutEnabled);
    const bool b2Enabled = b2LumEnabled || b2MemoryEnabled || r1GamutEnabled ||
        m1SkinEnabled || PerceptualEnabled();
    // A redraw after Pause is NOT a new original frame. Preserve the actual
    // presented LUT and gain, even when CompletedLast's fence has changed.
    // Do not enqueue a second model job for a paused redraw.
    const bool pausedB2Replay = b2Enabled && m_pFilter->m_filterState == State_Paused &&
        b.b2Presented.pts == pts && bool(b.b2Presented.view);
    if (pausedB2Replay) {
        lut = b.b2Presented.view.Get();
    } else if (m_pFilter->m_svdLutTemporal.load()) {
        PollSvdLutTemporalQueue();
        if (allowTemporalCapture) QueueSvdLutTemporalFrame(source, sampled, pts, fence);
        lut = SvdLutTemporalReadyView(pts, fence);
    } else {
        // Unmodified synchronous RAW B: deliberately measures model throughput.
        auto* entry = EvaluateSvdLutRawFrame(source, sampled, pts, fence);
        if (!entry) return S_FALSE;
        lut = SvdLutRawViewForFrame(*entry);
    }
    if (!lut) {
        if (m_bShowStats && m_showEditionDetails) b.grayProbe = {};
        return S_FALSE;
    }
    // Diagnostic-only selected-LUT gray probe. This is NOT a replacement
    // for a real pixel readback: it probes neutral sRGB(236,236,236) against
    // the CPU copy of the exact LUT bound to the pixel shader at this draw.
    // Opt-in through already existing Show statistics + Edition details.
    if (m_bShowStats && m_showEditionDetails && m_pFilter->m_svdLutTemporal.load()) {
        b.grayProbe = {};
        std::shared_ptr<const svdlut::Parameters> parameters;
        unsigned sourceKind = 0;
        for (const auto& frame : b.frames) {
            if (frame.temporalView.Get() == lut && frame.temporalParameters) {
                parameters = frame.temporalParameters;
                sourceKind = 1;
                break;
            }
            if (frame.view.Get() == lut && frame.parameters) {
                parameters = frame.parameters;
                break;
            }
        }
        if (!parameters && b.heldView.Get() == lut && b.heldParameters) {
            parameters = b.heldParameters;
            sourceKind = 2;
        }
        if (parameters) {
            constexpr float Gray = 236.0f / 255.0f;
            const std::array<float, 3> input{Gray, Gray, Gray};
            constexpr size_t Count = sizeof(svdlut::Parameters::lut) / sizeof(float);
            const auto packed = std::span<const float>(&parameters->lut[0][0], Count);
            const auto graded = svdlut::b2::ApplyB1(packed, input);
            const float t = edition::svdstrength::BBlend(m_pFilter->m_svdLutBStrength.load());
            auto& probe = b.grayProbe;
            probe.model = graded;
            for (size_t channel = 0; channel < 3; ++channel)
                probe.strengthMixed[channel] = input[channel] + (graded[channel] - input[channel]) * t;
            probe.pts = pts;
            probe.source = sourceKind;
            probe.valid = true;
        }
    }
    std::array<float, 3> b2Tone{1.0f, 1.0f, 1.0f};
    ID3D11PixelShader* drawShader = b.shader;
    if (b2LumEnabled) {
        b2Tone = SvdLutB2ToneForView(pts, fence, lut);
        if (b.b2LuminanceShader)
            drawShader = b.b2LuminanceShader;
    }
    // Colour protection must also work by itself (B2.1 unchecked). The
    // constants are then identity, so the same shader does not change tone.
    if (b2MemoryEnabled && b.b2MemoryShader)
        drawShader = b.b2MemoryShader;
    // R1 takes precedence when both toggles are checked, so turning R1 off
    // immediately returns to the user-approved B2.2 implementation.
    if (r1GamutEnabled && b.r1GamutShader)
        drawShader = b.r1GamutShader;
    // M1 replaces (not layers on top of) the selected B2.2/R1 shader.
    // Compile failure keeps the last known-good variant and is visible in OSD.
    if (m1SkinEnabled) {
        if (r1GamutEnabled && b.r1M1SkinShader)
            drawShader = b.r1M1SkinShader;
        else if (!r1GamutEnabled && b.m1SkinShader)
            drawShader = b.m1SkinShader;
    }
    // D1 shares the existing M1 shader + draw. Require an exact, unscaled
    // source-content rectangle: no guessed letterbox/SR/projected coordinate
    // transform. Gracefully skip depth when geometry or PTS is untrustworthy.
    const bool d1Requested = m1SkinEnabled && m_pFilter->m_svdLutD1Depth.load() &&
        (drawShader == b.m1SkinShader.p || drawShader == b.r1M1SkinShader.p);
    ID3D11ShaderResourceView* depthView = nullptr;
    float depthStrength = 0.0f, depthRotated = 0.0f;
    if (d1Requested && allowDepth && (UsesZipDepthSr() || UsesZipDepthMfg()) &&
        visible == area && offset == CPoint(0, 0) &&
        m_zipDepth.MatchesColorPicture(visible.Width(), visible.Height())) {
        const auto estimate = m_zipDepth.ForFrame(pts); // only an existing cache, NO inference
        const auto age = pts - estimate.pts;
        if (estimate.view && age >= 0 && age < 1'900'000) {
            // Full strength until 90 ms, then fade toward zero by 190 ms.
            // Prevent an abrupt hard gate on irregular/video-generated PTS.
            depthStrength = .75f * (std::min)(1.0f,
                float(1'900'000 - age) / 1'000'000.0f);
            depthView = estimate.view;
            depthRotated = estimate.rotated ? 1.0f : 0.0f;
        }
    }
    float cb[16] = { float(visible.left), float(visible.top),
        float(visible.Width() - 1), float(visible.Height() - 1),
        edition::svdstrength::BBlend(m_pFilter->m_svdLutBStrength.load()),
        float(offset.x), float(offset.y), depthRotated,
        b2Tone[0], b2Tone[1], b2Tone[2], depthStrength, 0, 0, 0, 0 };
    CRect srcRect(area);
    srcRect.OffsetRect(offset);
    // P1 takes precedence over the legacy repair experiments. The new
    // Preferred/Skin branches are independent of B2.2/R1 and share this draw.
    HRESULT hr = S_FALSE;
    if (PerceptualEnabled())
        hr = ApplyPerceptual(source, target, area, visible, offset, pts, 2, lut,
            b2Tone, b2LumEnabled, edition::svdstrength::BBlend(m_pFilter->m_svdLutBStrength.load()), allowDepth);
    if (hr != S_OK) {
        // Compile failure / disabled P1: preserve the previously tested shader.
        cb[12] = BindSvdLutOutputDither(target);
        m_pDeviceContext->UpdateSubresource(b.constants, 0, nullptr, cb, 0, 0);
        m_pDeviceContext->PSSetShaderResources(1, 1, &lut);
        if (depthView) m_pDeviceContext->PSSetShaderResources(2, 1, &depthView);
        m_pDeviceContext->PSSetSamplers(1, 1, &b.sampler.p);
        if (depthView) m_pDeviceContext->PSSetSamplers(2, 1, &b.sampler.p);
        hr = TextureCopyRect(source, target, srcRect, area, drawShader,
            b.constants, 0, false);
        ID3D11ShaderResourceView* clear = nullptr;
        ID3D11SamplerState* clearSampler = nullptr;
        m_pDeviceContext->PSSetShaderResources(1, 1, &clear);
        m_pDeviceContext->PSSetShaderResources(4, 1, &clear);
        if (depthView) m_pDeviceContext->PSSetShaderResources(2, 1, &clear);
        m_pDeviceContext->PSSetSamplers(1, 1, &clearSampler);
        if (depthView) m_pDeviceContext->PSSetSamplers(2, 1, &clearSampler);
    }
    b.failed = FAILED(hr);
    if (SUCCEEDED(hr) && d1Requested) {
        if (depthView && depthStrength > .001f) ++b.d1DepthDraws;
        else ++b.d1FallbackDraws;
    }
    b.active = SUCCEEDED(hr);
    if (b.active && m_pFilter->m_svdLutTemporal.load()) ++b.appliedDraws;
    if (b.active && b2Enabled) {
        auto& record = b.b2DrawRecords[b.b2DrawSlot++ % b.b2DrawRecords.size()];
        record.pts = pts;
        record.fence = fence;
        record.view = lut;
        const bool usedTone = (PerceptualEnabled() && b2LumEnabled) ||
            drawShader == b.b2LuminanceShader.p ||
            ((drawShader == b.b2MemoryShader.p || drawShader == b.r1GamutShader.p ||
              drawShader == b.m1SkinShader.p || drawShader == b.r1M1SkinShader.p) &&
             b2LumEnabled);
        record.tone = usedTone ? b2Tone :
            std::array<float, 3>{1.0f, 1.0f, 1.0f};
        if (usedTone) {
            svdlut::b2::Tone test{};
            test.gains = b2Tone;
            if (svdlut::b2::Adjusted(test)) ++b.b2AdjustedDraws;
        }
        if (drawShader == b.b2MemoryShader.p) ++b.b2MemoryDraws;
        if (drawShader == b.r1GamutShader.p) ++b.r1GamutDraws;
        if (drawShader == b.m1SkinShader.p || drawShader == b.r1M1SkinShader.p)
            ++b.m1SkinDraws;
    }
    return hr;
}

HRESULT CDX11VideoProcessor::DrawCompletedSvdLutRaw(const CompletedFrame& frame,
    ID3D11Texture2D* target, const CRect& picture, bool geometryChanging)
{
    auto& b = m_svdLutRawTest;
    b.active = false;
    if (!target || m_pFilter->m_svdLutBStrength.load() == 0 || b.failed || frame.pts == INVALID_TIME ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return S_FALSE;
    if (!b.attempted && !StartSvdLutRawTest()) return S_FALSE;
    if (b.model.GetState() != svdlut::ModelWorker::State::Ready) return S_FALSE;
    if (FAILED(PrepareSvdLutRawResources())) { b.failed = true; return S_FALSE; }
    D3D11_TEXTURE2D_DESC d{};
    target->GetDesc(&d);
    const CRect area(0, 0, d.Width, d.Height);
    CRect visible;
    if (!SupportedSdrFormat(d.Format) || !visible.IntersectRect(picture, area) ||
        visible.Width() < 2 || visible.Height() < 2) return S_FALSE;
    if (m_pFilter->m_svdLutTemporal.load()) {
        const bool pausedB2Replay =
            (m_pFilter->m_svdLutB2Luminance.load() || m_pFilter->m_svdLutB2Memory.load() ||
             m_pFilter->m_svdLutR1Gamut.load() || PerceptualEnabled()) &&
            m_pFilter->m_filterState == State_Paused &&
            b.b2Presented.pts == frame.pts && bool(b.b2Presented.view);
        if (!pausedB2Replay) {
            PollSvdLutTemporalQueue();
            // CompletedFrame::mfg is true for generated AND original MFG outputs.
            // Only phase == MaxGenerated identifies a real/original frame.
            // Sample content directly from the already-completed BGRA surface,
            // even if presentation will later resize/letterbox. This avoids
            // drawing an entire output-sized intermediate just to capture 256^2.
            // MFG generated frames (phase != MaxGenerated) borrow the original grade; otherwise
            // a 30->60 conversion doubles B inference work for little benefit.
            if (frame.phase == mfg::MaxGenerated && frame.image.pShaderResource) {
                CRect sampled;
                if (sampled.IntersectRect(frame.picture, CRect(0, 0,
                        frame.image.desc.Width, frame.image.desc.Height)) &&
                    sampled.Width() >= 2 && sampled.Height() >= 2)
                    QueueSvdLutTemporalFrame(frame.image, sampled, frame.pts, frame.fence);
            }
            // If no grade exists yet, allow the normal DrawCompletedVideo path
            // to render the original ONCE; do not draw it to b.picture and again
            // to the backbuffer during model warmup/slowdown.
            // Prefetch cannot depend on a previous grade already being visible;
            // it is needed most during warmup and after a backlog.
            PrefetchSvdLutRawFuture(frame, geometryChanging);
            if (!SvdLutTemporalReadyView(frame.pts, frame.fence)) return S_FALSE;
        }
    }
    const auto& input = frame.image.desc;
    if (frame.picture == picture && input.Width == d.Width && input.Height == d.Height &&
        input.Format == d.Format && frame.image.pShaderResource) {
        const auto hr = ApplySvdLutRawFrame(frame.image, target, area, visible, CPoint(0, 0), frame.pts, frame.fence, frame.phase == mfg::MaxGenerated,
            !geometryChanging && frame.picture == area);
        return hr;
    }
    if (FAILED(EnsureSvdLutRawPicture(d.Format, d.Width, d.Height))) { b.failed = true; return S_FALSE; }
    const auto drawn = DrawCompletedVideo(frame, b.picture.pTexture, picture, geometryChanging, &area);
    if (drawn != S_OK) return drawn;
    const auto hr = ApplySvdLutRawFrame(b.picture, target, area, visible, CPoint(0, 0), frame.pts, frame.fence, frame.phase == mfg::MaxGenerated);
    return hr;
}

void CDX11VideoProcessor::DrawSvdLutRaw(ID3D11Texture2D* target, const CRect& picture,
    REFERENCE_TIME pts)
{
    auto& b = m_svdLutRawTest;
    b.active = false;
    if (!target || m_pFilter->m_svdLutBStrength.load() == 0 || b.failed || pts == INVALID_TIME ||
        m_currentSwapChainColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        SourceIsHDR() || m_bVPUseRTXVideoHDR || m_pPSHalfOUtoInterlace) return;
    if (!b.attempted && !StartSvdLutRawTest()) return;
    if (b.model.GetState() != svdlut::ModelWorker::State::Ready) return;
    if (FAILED(PrepareSvdLutRawResources())) { b.failed = true; return; }
    D3D11_TEXTURE2D_DESC d{};
    target->GetDesc(&d);
    CRect visible;
    if (!SupportedSdrFormat(d.Format) ||
        !visible.IntersectRect(picture, CRect(0, 0, d.Width, d.Height)) ||
        visible.Width() < 2 || visible.Height() < 2) return;
    const UINT w = static_cast<UINT>(visible.Width());
    const UINT h = static_cast<UINT>(visible.Height());
    if (FAILED(EnsureSvdLutRawPicture(d.Format, w, h))) { b.failed = true; return; }
    const D3D11_BOX box{UINT(visible.left), UINT(visible.top), 0,
        UINT(visible.right), UINT(visible.bottom), 1};
    m_pDeviceContext->OMSetRenderTargets(0, nullptr, nullptr);
    m_pDeviceContext->CopySubresourceRegion(b.picture.pTexture, 0, 0, 0, 0, target, 0, &box);
    ApplySvdLutRawFrame(b.picture, target, visible, visible,
        CPoint(-visible.left, -visible.top), pts, 0);
}
