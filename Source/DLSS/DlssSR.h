// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <d3d11_1.h>
#include <memory>
#include <string>

struct Tex2D_t;

// DLSS Super Resolution on the renderer's D3D11 device.
class CDlssSR
{
public:
	CDlssSR();
	~CDlssSR();

	Tex2D_t* Prepare(ID3D11Device1* device, UINT width, UINT height, UINT outputWidth, UINT outputHeight);
	Tex2D_t* Evaluate(int64_t pts, float frameTimeMs);
	void ResetHistory();
	void Reset();
	bool Active() const;
	const wchar_t* Status() const;

private:
	struct Session;
	std::unique_ptr<Session> m_session;
	bool m_unavailable = false;
	std::wstring m_failure;
};
