// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <d3d11_1.h>
#include <memory>
#include <string>

struct Tex2D_t;
class CZipDepth;

// DLSS Super Resolution on the renderer's D3D11 device.
class CDlssSR
{
public:
	struct Stats {
		UINT inputWidth, inputHeight;
		UINT outputWidth, outputHeight;
		double inputFps;
		double ngxGpuMs; // Nonblocking D3D11 timestamp sample around NGX EvaluateFeature.
		bool ngxGpuTimeValid;
	};

	static constexpr UINT MaxOutputWidth = 7680;
	static constexpr UINT MaxOutputHeight = 4320;

	CDlssSR();
	~CDlssSR();

	Tex2D_t* Prepare(ID3D11Device1* device, UINT width, UINT height, UINT outputWidth, UINT outputHeight);
	Tex2D_t* Evaluate(int64_t pts, float frameTimeMs, CZipDepth* zipDepth, bool sourceFrame, ID3D11ShaderResourceView* cleanSource);
	void ResetHistory();
	void Reset();
	bool Active() const;
	Stats GetStats() const;
	int RequestedModel() const;
	const wchar_t* Status(const CZipDepth* zipDepth) const;

private:
	struct Session;
	std::unique_ptr<Session> m_session;
	bool m_unavailable = false;
	std::wstring m_failure;
    mutable std::wstring m_statusDetails;
};
