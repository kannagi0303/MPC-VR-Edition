// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CDX11VideoProcessor processing methods. Keep declaration order; this fragment is not a standalone header.
	bool DepthOverlayEnabled() const;
	bool BlendSourceDepthOverlay(ID3D11Texture2D* image, ID3D11ShaderResourceView* source,
		const CRect& picture, REFERENCE_TIME timestamp);
	void DrawDepthOverlay(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp);
	void DrawDepthView(ID3D11Texture2D* target, const CRect& picture, ID3D11ShaderResourceView* depth,
		SIZE textureSize, const CRect& source, int rotation, bool inverted);
	void DrawSubtitles(ID3D11Texture2D* pRenderTarget, const CRect* picture = nullptr);
	HRESULT Process(ID3D11Texture2D* pRenderTarget, const CRect& srcRect, const CRect& dstRect, bool second, bool allowDlssSR = true, REFERENCE_TIME timestamp = INVALID_TIME, CTex2DRing* processingPostScale = nullptr, bool* overlayInputHandled = nullptr, const Tex2D_t* preparedSource = nullptr);
	void ConfigureJvrProjection(const Settings_t& config);
	bool VideoEnhancementMayStart(bool buffered);
	float VideoEnhancementFade(REFERENCE_TIME pts) const;
	void ResetSvdLutHistory();
	void ReleaseSvdLut(bool fast = false);
	std::wstring SvdLutStatus() const;
	float BindSvdLutOutputDither(ID3D11Texture2D* target, bool enabled = true);
	HRESULT RenderSvdLutThumbnail(const Tex2D_t& source, ID3D11Texture2D* target, const CRect& sample);
	HRESULT EnsureSvdLutPicture(DXGI_FORMAT format, UINT width, UINT height);
	HRESULT DrawCompletedSvdLut(const CompletedFrame& frame, ID3D11Texture2D* target,
		const CRect& picture, bool geometryChanging);
	void DrawSvdLut(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp);
    // No-model fallback, or fused temporal grade + colour enhancement. Cached depth only.
    bool PerceptualEnabled() const;
    HRESULT PreparePerceptualShader(int mode);
    HRESULT ApplyPerceptual(const Tex2D_t& source, ID3D11Texture2D* target,
        const CRect& area, const CRect& visible, CPoint offset, REFERENCE_TIME pts,
        int mode, ID3D11ShaderResourceView* lut = nullptr,
        std::array<float, 3> tone = {1.0f, 1.0f, 1.0f}, bool useTone = false,
        float blend = 1.0f, bool allowDepth = false);
    HRESULT DrawCompletedPerceptual(const CompletedFrame& frame, ID3D11Texture2D* target,
        const CRect& picture, bool geometryChanging);
    void DrawPerceptual(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME pts);

	// Video Enhancement: asynchronous original-frame LUT analysis.
	void ReleaseSvdLutRawTest(bool fast = false);
	bool StartSvdLutRawTest();
	HRESULT PrepareSvdLutRawResources();
	HRESULT EnsureSvdLutRawPicture(DXGI_FORMAT format, UINT width, UINT height);
	void ResetSvdLutTemporalQueue();
	void PollSvdLutTemporalQueue();
	void PrimeSvdLutTemporalFrames();
	SvdLutRawFrame* QueueSvdLutTemporalFrame(const Tex2D_t& source,
		const CRect& sample, REFERENCE_TIME pts, uint64_t fence);
	ID3D11ShaderResourceView* SvdLutTemporalReadyView(REFERENCE_TIME pts, uint64_t fence);
    std::array<float, 3> SvdLutB2ToneForView(REFERENCE_TIME pts, uint64_t fence,
        ID3D11ShaderResourceView* selected);
    void RememberPresentedSvdLutB2(REFERENCE_TIME pts, uint64_t fence);
	ID3D11ShaderResourceView* SvdLutRawViewForFrame(SvdLutRawFrame& frame);
	void PrefetchSvdLutRawFuture(const CompletedFrame& current, bool geometryChanging);
	HRESULT ApplySvdLutRawFrame(const Tex2D_t& source, ID3D11Texture2D* target,
		const CRect& area, const CRect& visible, CPoint offset, REFERENCE_TIME pts, uint64_t fence,
		bool allowTemporalCapture = true, bool allowDepth = false);
	HRESULT DrawCompletedSvdLutRaw(const CompletedFrame& frame, ID3D11Texture2D* target,
		const CRect& picture, bool geometryChanging);
	void DrawSvdLutRaw(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME pts);
	bool CanProjectJvr() const;
	HRESULT PrepareJvrFrame(const CRect& picture, bool second);
	HRESULT ProjectJvr(const Tex2D_t& source, ID3D11Texture2D* target, const CRect& sourceRect, const CRect& picture);
    bool DlssSRSourceAllowed(const CRect* preparedRect = nullptr) const;
	bool CanDlssSRUpscale(const CRect& srcRect, const CRect& dstRect, bool preparedRgb = false);
	Tex2D_t* PrepareDlssSR(const CRect& srcRect, const CRect& dstRect, bool afterMfg);
	Tex2D_t* EvaluateDlssSR(const Tex2D_t& source, Tex2D_t& input, const CRect& sourceRect, REFERENCE_TIME timestamp, float frameTimeMs, bool* overlayInputHandled = nullptr, bool depthPrepared = false);
	Tex2D_t* TryDlssSR(const CRect& srcRect, const CRect& dstRect, bool second, REFERENCE_TIME timestamp, bool* overlayInputHandled, const Tex2D_t* preparedSource);
