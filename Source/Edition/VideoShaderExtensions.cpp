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
#include <cmath>
#include <numbers>
#include "Helper.h"
#include "Shaders.h"
#include "resource.h"

// A compute shader has no implicit derivatives, so it cannot call Sample. The pictures
// carry a single level, so naming that level reads the same texels: Sample(s, uv)
// becomes SampleLevel(s, uv, 0), and the offset, where there is one, stays last.
static void SampleAtTopLevel(std::string& code)
{
	constexpr std::string_view call = ".Sample(";
	constexpr std::string_view level = ".SampleLevel(";

	for (size_t at = code.find(call); at != std::string::npos; at = code.find(call, at)) {
		code.replace(at, call.length(), level);

		size_t pos = at + level.length();
		unsigned depth = 1, commas = 0;
		while (pos < code.length()) {
			const char c = code[pos];
			if (c == '(') {
				depth++;
			}
			else if (c == ')' && !--depth) {
				break;
			}
			else if (c == ',' && depth == 1 && ++commas == 2) {
				break;   // the offset follows, and the level goes before it
			}
			pos++;
		}
		code.insert(pos, ", 0");
		at = pos;
	}
}

// The polar kernel madVR and mpv call Jinc: jinc(d), windowed by a jinc stretched
// to the radius, cut at 3.2383 -- the third zero of jinc, mpv's ewa_lanczos. It is
// not separable, so the weights are a disc around each point, not a row and a column.
static double JincWindowed(const double d)
{
	constexpr double kRadius = 3.2383;      // the third zero of jinc
	constexpr double kFirstZero = 1.2196699;
	if (d >= kRadius) {
		return 0.0;
	}
	const auto jinc = [](const double x) {
		if (x < 1e-8) {
			return 1.0;
		}
		const double t = std::numbers::pi * x;
		return 2.0 * std::cyl_bessel_j(1.0, t) / t;
	};
	return jinc(d) * jinc(d * kFirstZero / kRadius);
}

// The same kernel on 4:2:0 chroma, written out. Chroma sits at a fixed place among
// the luma pixels, so a pixel's position inside its chroma texel depends only on the
// parity of its coordinates: four sets of weights, known here once and for all, and
// the shader then only adds texels up. `sample` is the texel at an offset, `{}` twice
// for the offset; `target` is what the sum goes into.
//
// The four sets go through one sum, each texel's weight picked by the parity. Every 2x2
// block of pixels holds one pixel of each parity, so a branch per parity had each
// group of pixels run all four of them, a quarter of its pixels at a time: 130 texture
// reads in the shader and 1.10 ms a picture at 1080p on an RTX 4060, against 43 reads
// and 0.36 ms now (RenderDoc). A weight a parity does not use is a zero there, and the
// sums run in the same order as before; the GPU's compiler still arranges them its own
// way, and the 8-bit picture differs in 15 of its 8.3 million values, by one level.
void edition::AppendJincChroma420(const UINT chromaSubsampling, const char* sample, const char* target,
	std::string& code)
{
	// Where the chroma sample sits, in chroma texels, from the centred position.
	const double sx = (chromaSubsampling == DXVA2_VideoChromaSubsampling_MPEG1) ? 0.0 : 0.25;
	const double sy = (chromaSubsampling == DXVA2_VideoChromaSubsampling_Cosited) ? 0.25 : 0.0;
	constexpr int kReach = 4;
	constexpr int kSide = 2 * kReach + 1;

	// [parity: x + 2y][row][column], normalized.
	double weights[4][kSide][kSide] = {};
	for (int py = 0; py < 2; py++) {
		for (int px = 0; px < 2; px++) {
			auto& w = weights[px + 2 * py];
			double total = 0;
			for (int j = -kReach; j <= kReach; j++) {
				for (int i = -kReach; i <= kReach; i++) {
					const double dx = i + 0.5 - (px + 0.5) / 2 - sx;
					const double dy = j + 0.5 - (py + 0.5) / 2 - sy;
					const double weight = JincWindowed(std::sqrt(dx * dx + dy * dy));
					w[j + kReach][i + kReach] = weight;
					total += weight;
				}
			}
			for (auto& row : w) {
				for (double& weight : row) {
					weight /= total;
				}
			}
		}
	}

	code.append(
		"uint2 jinc_parity = uint2(input.Tex * wh) & 1u;\n"
		"const float4 jinc_pick = float4(jinc_parity.x == 0u && jinc_parity.y == 0u, jinc_parity.x == 1u && jinc_parity.y == 0u,\n"
		"                                jinc_parity.x == 0u && jinc_parity.y == 1u, jinc_parity.x == 1u && jinc_parity.y == 1u);\n");
	code += std::format("{} = 0;\n", target);
	for (int j = -kReach; j <= kReach; j++) {
		for (int i = -kReach; i <= kReach; i++) {
			const double* w[4] = {};
			bool bUsed = false;
			for (int p = 0; p < 4; p++) {
				w[p] = &weights[p][j + kReach][i + kReach];
				bUsed = bUsed || *w[p] != 0.0;
			}
			if (!bUsed) {
				continue;
			}
			code += std::format("{} += dot(float4({:.8f}, {:.8f}, {:.8f}, {:.8f}), jinc_pick) * ",
				target, *w[0], *w[1], *w[2], *w[3]);
			code += std::vformat(sample, std::make_format_args(i, j));
			code.append(";\n");
		}
	}
}

HRESULT GetShaderConvertTo444(
	const UINT width,
	const long texW, const long texH,
	const FmtConvParams_t& fmtParams,
	const DXVA2_ExtendedFormat exFmt,
	const int chromaScaling,
	const DXGI_FORMAT packedFormat,
	ID3DBlob** ppCode)
{
	DLog(L"GetShaderConvertTo444() started for {} {}x{} to {} chroma:{}", fmtParams.str, texW, texH,
		DXGIFormatToString(packedFormat), chromaScaling);

	const bool packed422 = (fmtParams.cformat == CF_YUY2 || fmtParams.cformat == CF_UYVY
		|| fmtParams.cformat == CF_Y210
		|| fmtParams.cformat == CF_Y216
		|| fmtParams.cformat == CF_v210);
	const bool fix422 = (packed422 && texW * 2 == width);

	std::string code;
	code += std::format("#define w {}\n", fix422 ? width : texW);
	code += std::format("#define dx (1.0/{})\n", texW);
	code += std::format("#define dy (1.0/{})\n", texH);
	code += std::format("static const float2 wh = {{{}, {}}};\n", fix422 ? width : texW, texH);
	code += std::format("static const float2 dxdy2 = {{2.0/{}, 2.0/{}}};\n", texW, texH);

	ShaderGetPixels(true, fmtParams, exFmt.VideoChromaSubsampling, chromaScaling, false, packedFormat, code);
	SampleAtTopLevel(code);

	// color now holds Y, Cb and Cr as the source stores them, 0 to 1 of its own scale.
	// The video processor reads the studio range as 16/255 to 235/255 of the full
	// scale whatever the depth, so the code written is the source's value brought to
	// that scale: the same multiplier for luma and chroma, and nothing else to do.
	const int maxCode = (packedFormat == DXGI_FORMAT_AYUV) ? 255 : 1023;
	double srcBlack = 16.0 / 255.0, srcWhite = 235.0 / 255.0;
	if (fmtParams.CDepth == 10) {
		srcBlack = 64.0 / 1023.0;
		srcWhite = 940.0 / 1023.0;
	} else if (fmtParams.CDepth > 10) {
		srcBlack = 4096.0 / 65535.0;   // ten bits in the high bits of sixteen
		srcWhite = 60160.0 / 65535.0;
	}
	// A full-range source is handed over as it is: the processor is told so and reads
	// the whole scale, so there is nothing to stretch.
	const bool fullrange = (exFmt.NominalRange == DXVA2_NominalRange_0_255);
	const double scale = fullrange ? maxCode : maxCode * (235.0 - 16.0) / 255.0 / (srcWhite - srcBlack);

	code += std::format("const uint3 q = (uint3)clamp(round(color.rgb * {}), 0, {});\n", scale, maxCode);
	if (packedFormat == DXGI_FORMAT_AYUV) {
		// V, U, Y, A through an R8G8B8A8 view, so V is the low byte.
		code.append("packed[threadId.xy] = q.z | (q.y << 8) | (q.x << 16) | (255u << 24);\n");
	} else {
		// Y410: U, Y, V, A through an R10G10B10A2 view.
		code.append("packed[threadId.xy] = q.y | (q.x << 10) | (q.z << 20) | (3u << 30);\n");
	}
	code.append("}\n");

	return CompileShader(code, nullptr, "cs_5_0", ppCode);
}

HRESULT GetShaderRgbToVideo(
	const int part,
	const bool bBT2020,
	const UINT width, const UINT height,
	const double scale10,
	ID3DBlob** ppCode)
{
	DLog(L"GetShaderRgbToVideo() started for part {} {}x{} {}", part, width, height, bBT2020 ? L"BT.2020" : L"BT.709");

	const double kr = bBT2020 ? 0.2627 : 0.2126;
	const double kb = bBT2020 ? 0.0593 : 0.0722;
	const double kg = 1.0 - kr - kb;

	std::string code;
	code += "Texture2D<float4> tex : register(t0);\n";
	code += std::format("static const int2 last = int2({}, {});\n", width - 1, height - 1);
	code += std::format("static const float3 kY = float3({}, {}, {});\n", kr, kg, kb);
	// The texture written may be a pixel larger than the picture, to be even: what
	// lies past the picture's edge repeats it.
	code += "float3 Pixel(int2 p) { return tex.Load(int3(clamp(p, int2(0, 0), last), 0)).rgb; }\n";
	// Y', Cb and Cr in levels of the 8-bit scale, studio range.
	code += std::format(
		"float3 ToYCbCr(float3 c)\n"
		"{{\n"
		"	const float y = dot(c, kY);\n"
		"	return float3(16.0 + 219.0 * y, 128.0 + 224.0 * (c.b - y) / {}, 128.0 + 224.0 * (c.r - y) / {});\n"
		"}}\n", 2.0 * (1.0 - kb), 2.0 * (1.0 - kr));

	if (part == RGBTOVIDEO_Packed) {
		// Y410: U, Y, V, A through an R10G10B10A2 view.
		code += "RWTexture2D<uint> packed : register(u0);\n"
			"[numthreads(8, 8, 1)]\n"
			"void main(uint3 id : SV_DispatchThreadID)\n"
			"{\n";
		code += std::format("	const uint3 q = (uint3)clamp(round(ToYCbCr(Pixel(int2(id.xy))) * {}), 0.0, 1023.0);\n", scale10);
		code += "	packed[id.xy] = q.y | (q.x << 10) | (q.z << 20) | (3u << 30);\n"
			"}\n";
		return CompileShader(code, nullptr, "cs_5_0", ppCode);
	}

	// P010 keeps its ten bits in the high bits of sixteen.
	const double unorm = scale10 * 64.0 / 65535.0;
	code += "struct PS_INPUT { float4 Pos : SV_POSITION; float2 Tex : TEXCOORD; };\n"
		"float4 main(PS_INPUT input) : SV_Target\n"
		"{\n";
	if (part == RGBTOVIDEO_Luma) {
		code += std::format("	return float4(ToYCbCr(Pixel(int2(input.Pos.xy))).x * {}, 0, 0, 1);\n", unorm);
	} else {
		// One chroma sample for two columns and two rows, where MPEG-2 puts it: on the
		// even column, between the two rows. 1 2 1 across, the two rows alike.
		code += "	const int2 p = int2(input.Pos.xy) * 2;\n"
			"	float3 c = 0;\n"
			"	for (int r = 0; r < 2; r++) {\n"
			"		c += 0.125 * Pixel(p + int2(-1, r)) + 0.25 * Pixel(p + int2(0, r)) + 0.125 * Pixel(p + int2(1, r));\n"
			"	}\n";
		code += std::format("	return float4(ToYCbCr(c).yz * {}, 0, 1);\n", unorm);
	}
	code += "}\n";

	return CompileShader(code, nullptr, "ps_4_0", ppCode);
}
