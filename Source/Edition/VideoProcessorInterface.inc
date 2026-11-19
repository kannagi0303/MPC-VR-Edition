// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CVideoProcessor extension virtual interface. Keep declaration order; this fragment is not a standalone header.
	// One line describing the DLSS 5 NR session; empty when not applicable.
	virtual std::wstring GetDlssStatus() { return {}; }
	virtual std::wstring GetMfgStatus() { return L"MFG unavailable on this processor"; }
	virtual std::wstring GetDlssSRStatus() { return {}; }

	// Render ahead: how much earlier than usual the next sample should be processed,
	// in 100 ns units, and how long pictures were held for their time since the last
	// call. See CRenderAhead.
	virtual int GetRenderAhead() { return 0; }
	REFERENCE_TIME TakeHeldTime() { const REFERENCE_TIME held = m_rtHeld; m_rtHeld = 0; return held; }

	// The statistics as drawn on the picture; empty where the processor has none.
	virtual std::wstring GetStatsText() { return {}; }

	// Hiding a channel's logo (DX11 only): a new analysis, the logos kept forgotten, and
	// what it is doing, for the settings page; the logos kept for the film playing, one a
	// line, and the one chosen among them (0: found or recognised by itself).
	virtual void FindLogo() {}
	virtual void ForgetLogos() {}
	virtual std::wstring GetLogoStatus() { return {}; }
	virtual std::wstring GetLogoList() { return {}; }
	virtual int GetLogoChoice() { return 0; }
	virtual void ChooseLogo(int) {}

