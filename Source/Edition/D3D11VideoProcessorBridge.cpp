/*
* (C) 2019-2026 see Authors.txt
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
#include "IVideoRenderer.h"
#include "DX11Helper.h"
#include "D3D11VP.h"
#include <iterator>

HRESULT CD3D11VP::CheckInputTexture(ID3D11Texture2D* texture) const
{
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
	view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	return m_pVideoDevice->CreateVideoProcessorInputView(texture, m_pVideoProcessorEnum, &view, nullptr);
}

CD3D11VP::HeldFrame CD3D11VP::HoldFrame()
{
	HeldFrame frame;
	if (!m_bPresentFrame) {
		return frame;
	}

	if (!m_bDecoderInput && m_VideoTextures.Size()) {
		// An uploaded frame: the newest copy is at the back of the ring.
		frame.pTexture = *m_VideoTextures.GetTexture();
	}
	else if (m_bDecoderInput && m_VideoInputData.GetLastSample()) {
		// Decoder output is used in place; holding the sample keeps its surface.
		frame.pTexture   = m_VideoInputData.GetTexture();
		frame.pSample    = m_VideoInputData.GetLastSample();
		frame.ArraySlice = m_nLastArraySlice;
	}

	return frame;
}

void CD3D11VP::RestoreFrame(const HeldFrame& frame, ID3D11DeviceContext* pContext, const D3D11_VIDEO_FRAME_FORMAT vframeFormat)
{
	if (!frame.pTexture || !IsReady()) {
		return;
	}

	if (frame.pSample) {
		SetInputVideoData(frame.pTexture, frame.pSample, frame.ArraySlice, vframeFormat);
		return;
	}

	ID3D11Texture2D* pRingTexture = m_VideoTextures.Size() ? *m_VideoTextures.GetTexture() : nullptr;
	if (!pRingTexture || !pContext) {
		return;
	}

	// Only into input textures of the same shape: anything else means the
	// rebuild changed more than the input it was supposed to keep.
	D3D11_TEXTURE2D_DESC held = {};
	D3D11_TEXTURE2D_DESC ring = {};
	frame.pTexture->GetDesc(&held);
	pRingTexture->GetDesc(&ring);
	if (held.Format != ring.Format || held.Width != ring.Width || held.Height != ring.Height) {
		return;
	}

	pContext->CopyResource(GetNextInputTexture(vframeFormat), frame.pTexture);
}

// Use the native DXVA2 -> D3D11 mapping, with the independent VP's own ranges.
// A missing filter has a zero-initialized range; never divide by its multiplier.
int ValueDXVA2toD3D11(const DXVA2_Fixed32 fixed, const D3D11_VIDEO_PROCESSOR_FILTER_RANGE& range);
void CD3D11VP::SetExternalProcAmpValues(const DXVA2_ProcAmpValues& values)
{
    const DXVA2_Fixed32 requested[] = {values.Brightness, values.Contrast, values.Hue, values.Saturation};
    for (UINT i = 0; i < std::size(requested); ++i) {
        auto& filter = m_VPFilters[i];
        if (filter.support && filter.range.Multiplier > 0.0f) {
            filter.value = ValueDXVA2toD3D11(requested[i], filter.range);
        } else {
            filter.value = filter.range.Default;
        }
    }
    m_bUpdateFilters = true;
}

HRESULT CD3D11VP::ProcessExternal(ID3D11Texture2D* pInput, UINT arraySlice,
    ID3D11Texture2D* pRenderTarget, const RECT& srcRect, const RECT& dstRect)
{
    if (!pInput || !pRenderTarget) return E_POINTER;
    if (!m_pVideoDevice || !m_pVideoContext || !m_pVideoProcessor || !m_pVideoProcessorEnum)
        return E_NOT_VALID_STATE;
    D3D11_TEXTURE2D_DESC inputDesc{}, outputDesc{};
    pInput->GetDesc(&inputDesc);
    pRenderTarget->GetDesc(&outputDesc);
    if (inputDesc.Format != m_srcFormat || arraySlice >= inputDesc.ArraySize ||
        inputDesc.Width < m_srcWidth || inputDesc.Height < m_srcHeight ||
        outputDesc.Format != m_dstFormat ||
        srcRect.left < 0 || srcRect.top < 0 ||
        srcRect.right > LONG(inputDesc.Width) || srcRect.bottom > LONG(inputDesc.Height) ||
        srcRect.right <= srcRect.left || srcRect.bottom <= srcRect.top ||
        dstRect.left < 0 || dstRect.top < 0 ||
        dstRect.right > LONG(outputDesc.Width) || dstRect.bottom > LONG(outputDesc.Height) ||
        dstRect.right <= dstRect.left || dstRect.bottom <= dstRect.top)
        return E_INVALIDARG;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.ArraySlice = arraySlice;
    CComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = m_pVideoDevice->CreateVideoProcessorInputView(
        pInput, m_pVideoProcessorEnum, &inputViewDesc, &inputView);
    if (FAILED(hr)) return hr;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    CComPtr<ID3D11VideoProcessorOutputView> outputView;
    hr = m_pVideoDevice->CreateVideoProcessorOutputView(
        pRenderTarget, m_pVideoProcessorEnum, &outputViewDesc, &outputView);
    if (FAILED(hr)) return hr;

    hr = SetRectangles(&srcRect, &dstRect);
    if (FAILED(hr)) return hr;
    m_pVideoContext->VideoProcessorSetStreamFrameFormat(
        m_pVideoProcessor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    // Match the native first-field filter commit. External RGB upscalers
    // and neutral lookahead VPs never receive ProcAmp updates.
    if (m_bUpdateFilters) {
        for (UINT i = 0; i < std::size(m_VPFilters); ++i) {
            const auto& filter = m_VPFilters[i];
            if (filter.support && filter.range.Multiplier > 0.0f) {
                m_pVideoContext->VideoProcessorSetStreamFilter(m_pVideoProcessor, 0,
                    (D3D11_VIDEO_PROCESSOR_FILTER)i, filter.value != filter.range.Default, filter.value);
            }
        }
        m_bUpdateFilters = false;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.InputFrameOrField = m_nInputFrameOrField++;
    stream.pInputSurface = inputView;
    // This immediate-context call is ordered with D3D11 input copies and the
    // downstream Present. Do not wait on the CPU or hold a decoder sample.
    return m_pVideoContext->VideoProcessorBlt(
        m_pVideoProcessor, outputView, stream.InputFrameOrField, 1, &stream);
}


// Product selection policy is separate from VP input/history correctness.
// Empty means the native threshold and vendor dispatch should continue.
std::optional<HRESULT> CD3D11VP::ApplyEditionSuperResPolicy(int mode)
{
	if (m_VendorId == PCIV_INTEL) return E_NOT_SET;
	if (m_VendorId == PCIV_NVIDIA && mode == SUPERRES_Always) return SetSuperResNvidia(true);
	return {};
}
