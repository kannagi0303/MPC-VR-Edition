// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../Shaders.h"

// A compute shader that rebuilds the chroma the same way and writes the picture as
// packed 4:4:4 -- AYUV for an 8-bit source, Y410 above -- so that the D3D11 video
// processor, which reads it next, has no chroma of its own to rebuild. The picture
// stays in Y'CbCr, brought to the scale the processor reads studio range on, and the
// conversion to RGB is still its own.
HRESULT GetShaderConvertTo444(
	const UINT width,
	const long texW, const long texH,
	const FmtConvParams_t& fmtParams,
	const DXVA2_ExtendedFormat exFmt,
	const int chromaScaling,
	const DXGI_FORMAT packedFormat,
	ID3DBlob** ppCode);

// The way back: the R'G'B' picture a DLSS pass leaves, written as the Y'CbCr the D3D11
// video processor takes, so that the processor can enlarge it after the pass. Nothing
// is converted but the matrix -- the transfer function and the primaries stay as they
// are, and the processor is told the picture is in the space it already is in.
//
// Three shaders. P010, which the driver's Super Resolution acts on, is written a plane
// at a time: RGBTOVIDEO_Luma into the R16 view of the texture, RGBTOVIDEO_Chroma into
// its R16G16 view at half the size, the chroma sited as MPEG-2 sites it. Y410, for a
// picture whose chroma the shaders were asked to keep whole, is one compute shader
// through an R32_UINT view, as GetShaderConvertTo444 writes it.
//
// width and height are the picture's; scale10 is how many codes of the 10-bit scale a
// level of the 8-bit one is written as (4, or 1023/255 where a driver reads the studio
// range from the full scale).
enum : int {
	RGBTOVIDEO_Luma = 0,
	RGBTOVIDEO_Chroma,
	RGBTOVIDEO_Packed,
};
HRESULT GetShaderRgbToVideo(
	const int part,
	const bool bBT2020,
	const UINT width, const UINT height,
	const double scale10,
	ID3DBlob** ppCode);

// Native pixel reconstruction shared with the Edition compute output adapter.
void ShaderGetPixels(bool bDX11, const FmtConvParams_t& fmtParams, UINT chromaSubsampling,
	int chromaScaling, bool blendDeinterlace, DXGI_FORMAT packedFormat, std::string& code);
namespace edition {
void AppendJincChroma420(UINT chromaSubsampling, const char* sample, const char* target, std::string& code);
}
