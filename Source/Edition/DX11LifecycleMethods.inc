// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CDX11VideoProcessor private lifecycle methods.
	const Tex2D_t* SelectEditionProcessingSource(const CRect& srcRect, const CRect& dstRect,
		bool second, bool allowDlssSR, REFERENCE_TIME timestamp, bool* overlayInputHandled,
		const Tex2D_t* preparedSource, CRect& processingSourceRect);
	std::optional<HRESULT> PrepareEditionRender(edition::VideoFrame& frame,
		CComPtr<ID3D11Texture2D>& pBackBuffer, uint64_t& tick1, HRESULT& hr);
	std::optional<size_t> AppendEditionStatistics(std::wstring& str, double sourceFps);
	void InsertEditionStatsSeparator(std::wstring& str, size_t separator);
	void InitializeEdition(Settings_t& config);
	void BeginEditionShutdown();
	std::optional<HRESULT> RenderEditionVideo(CComPtr<ID3D11Texture2D>& pBackBuffer,
		const edition::VideoFrame& frame, HRESULT& hr);
	struct EditionConfiguration { int superRes; bool srChanged; bool jvrModeChanged; };
	static void ApplyJvrPlaybackMode(Settings_t& config, bool projectJvr);
	EditionConfiguration ConfigureEdition(Settings_t& config);
	void ResetEditionVideoProcessor();
	void StopEditionDevice();
	void ReleaseEditionProjectionResources();
	void ReleaseEditionOverlayResources();
	void FlushEdition();
	void BeginEditionSample(IMediaSample* sample);
	void AnalyzeEditionDecodedFrame(HRESULT hr, bool comparedCpuNv12);
	bool AnalyzeEditionCpuNv12(const BYTE* data, HRESULT hr);
	void AppendEditionProcessorInfo(std::wstring& str);
	bool PrepareEditionSubtitleShader();
