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

	Tex2D_t* Prepare(ID3D11Device1* device, UINT width, UINT height, UINT outputWidth, UINT outputHeight, bool background);
	Tex2D_t* Evaluate(int64_t pts, float frameTimeMs, CZipDepth* zipDepth, bool sourceFrame, ID3D11ShaderResourceView* cleanSource);
	bool Preparing() const;
	void ResetHistory();
	void Reset();
	bool Active() const;
	Stats GetStats() const;
	int RequestedModel() const;
	const wchar_t* Status(const CZipDepth* zipDepth) const;

private:
	struct Session;
	struct Preparation;
	static bool OpenSession(Session& session, ID3D11Device1* device, bool background);
	static void CreateSession(Session& session, UINT width, UINT height, UINT outputWidth,
		UINT outputHeight, bool smallerOnly);
	void StartPreparation(std::unique_ptr<Session> retired, ID3D11Device1* device,
		UINT width, UINT height, UINT outputWidth, UINT outputHeight, bool smallerOnly);
	std::unique_ptr<Session> m_session;
	std::unique_ptr<Preparation> m_preparation;
	bool m_retrySmaller = false;
	bool m_unavailable = false;
	std::wstring m_failure;
    mutable std::wstring m_statusDetails;
};
