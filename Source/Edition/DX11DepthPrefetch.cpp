/*
* (C) 2018-2026 see Authors.txt
*
* This file is part of MPC-BE.
*
* MPC-BE is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 3 of the License, or
* (at your option) any later version.
*
* MPC-BE is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#include "stdafx.h"
#include "Helper.h"
#include "DX11VideoProcessor.h"
#include <new>
#include <ID3DVideoMemoryConfiguration.h>

namespace {
class LookaheadSample final : public CMediaSample, public IMediaSampleD3D11 {
    CComPtr<CMemAllocator> m_allocator;
    CComPtr<ID3D11Texture2D> m_texture;
public:
    LookaheadSample(CMemAllocator* allocator, ID3D11Texture2D* texture)
        : CMediaSample(L"depth lookahead sample", allocator, nullptr), m_allocator(allocator), m_texture(texture) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IMediaSampleD3D11)) return GetInterface(static_cast<IMediaSampleD3D11*>(this), out);
        return CMediaSample::QueryInterface(iid, out);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return CMediaSample::AddRef(); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&m_cRef);
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP GetD3D11Texture(int view, ID3D11Texture2D** texture, UINT* slice) override {
        if (!texture || !slice) return E_POINTER;
        if (view) return E_INVALIDARG;
        *slice = 0;
        return m_texture.CopyTo(texture);
    }
};
}

CComPtr<IMediaSample> CDX11VideoProcessor::CloneLookaheadSample(IMediaSample* sample, int64_t& pts, uint64_t& bytes)
{
    if (CanProjectJvr() || !m_pDeviceContext || !m_pDevice || (!UsesDepthMfg() && !UsesDepthSr()) ||
        !MfgDepthSourceEligible() || m_bInterlaced || m_srcDXGIFormat != DXGI_FORMAT_NV12 ||
        m_srcExFmt.VideoTransferMatrix != DXVA2_VideoTransferMatrix_BT709 ||
        sample->IsDiscontinuity() == S_OK || sample->IsPreroll() == S_OK)
        return {};
    REFERENCE_TIME end = 0;
    if (sample->GetTime(&pts, &end) != S_OK || pts < 0 || end <= pts) return {};
    AM_MEDIA_TYPE* changedType = nullptr;
    if (sample->GetMediaType(&changedType) == S_OK) {
        DeleteMediaType(changedType);
        return {};
    }
    if (CComQIPtr<IMediaSideData> sideData = sample) {
        const BYTE* data = nullptr;
        size_t size = 0;
        if (SUCCEEDED(sideData->GetSideData(IID_MediaSideDataDOVIMetadataV2, &data, &size)) && size) return {};
    }
    const CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> input;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &input, &slice)) || !input) return {};
    CComPtr<ID3D11Device> owner;
    input->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return {};
    D3D11_TEXTURE2D_DESC desc{};
    input->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || desc.MipLevels != 1 || slice >= desc.ArraySize ||
        desc.Width < m_srcWidth || desc.Height < m_srcHeight)
        return {};
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (m_D3D11VP.IsReady()) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    CComPtr<ID3D11Texture2D> copy;
    if (FAILED(m_pDevice->CreateTexture2D(&desc, nullptr, &copy))) return {};
    // Check the VP view before acknowledging a sample as queued for later use.
    if (m_D3D11VP.IsReady() && FAILED(m_D3D11VP.CheckInputTexture(copy))) return {};
    if (!m_lookaheadAllocator) {
        HRESULT hr = S_OK;
        auto* allocator = new(std::nothrow) CMemAllocator(L"depth lookahead", nullptr, &hr);
        if (!allocator || FAILED(hr)) {
            delete allocator;
            return {};
        }
        m_lookaheadAllocator = allocator;
    }
    auto* packet = new(std::nothrow) LookaheadSample(m_lookaheadAllocator, copy);
    if (!packet) return {};
    CComPtr<IMediaSample> result(static_cast<IMediaSample*>(packet));
    packet->SetTime(&pts, &end);
    packet->SetSyncPoint(sample->IsSyncPoint() == S_OK);
    LONGLONG mediaStart = 0, mediaEnd = 0;
    if (sample->GetMediaTime(&mediaStart, &mediaEnd) == S_OK) packet->SetMediaTime(&mediaStart, &mediaEnd);
    m_pDeviceContext->CopySubresourceRegion(copy, 0, 0, 0, 0, input, slice, nullptr);
    bytes = uint64_t(desc.Width) * desc.Height * 3 / 2;
    return result;
}

bool CDX11VideoProcessor::PrefetchDepth(IMediaSample* sample, int64_t pts)
{
    if (CanProjectJvr() || (!UsesDepthMfg() && !UsesDepthSr()) || !m_pDevice || !m_pDeviceContext || !MfgDepthSourceEligible())
        return true;
    CComQIPtr<IMediaSampleD3D11> decoded(sample);
    CComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!decoded || FAILED(decoded->GetD3D11Texture(0, &texture, &slice)) || !texture || slice) return true;
    CComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.p != static_cast<ID3D11Device*>(m_pDevice.p)) return true;
    return m_depth.PrefetchNv12(m_pDevice, m_pDeviceContext, texture, m_srcRect,
        m_srcExFmt.NominalRange == DXVA2_NominalRange_0_255, pts);
}


void CDX11VideoProcessor::AnalyzeEditionDecodedFrame(HRESULT hr, bool comparedCpuNv12)
{
	if (!comparedCpuNv12 && !CanProjectJvr() && MfgFrameAnalysisEligible() && SUCCEEDED(hr)) {
		if (m_D3D11VP.IsReady()) {
			const auto held = m_D3D11VP.HoldFrame();
			m_mfgDecodedDifference.Submit(m_pDevice, m_pDeviceContext, held.pTexture,
				held.ArraySlice, m_srcRect, m_rtStart);
		} else {
			m_mfgDecodedDifference.Submit(m_pDevice, m_pDeviceContext, m_TexSrcVideo.pTexture,
				0, m_srcRect, m_rtStart);
		}
	}

}

bool CDX11VideoProcessor::AnalyzeEditionCpuNv12(const BYTE* data, HRESULT hr)
{
			if (SUCCEEDED(hr) && !CanProjectJvr() && MfgFrameAnalysisEligible() && m_srcDXGIFormat == DXGI_FORMAT_NV12 &&
			    m_srcPitch > 0 && m_srcLines >= m_srcHeight + m_srcHeight / 2) {
				m_mfgDecodedDifference.SubmitCpuNv12(data, m_srcPitch, m_srcHeight, m_srcRect, m_rtStart, m_mfgLowMotionEnabled);
				return true;
			}
	return false;
}
