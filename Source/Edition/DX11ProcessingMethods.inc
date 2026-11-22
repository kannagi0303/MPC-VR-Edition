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
	void SetSvdLutStrength(int strength);
	void ResetSvdLutHistory();
	void ReleaseSvdLut(bool fast = false);
	std::wstring SvdLutStatus() const;
	HRESULT PrepareSvdLutDraw();
	bool StartSvdLutModel();
	HRESULT EnsureSvdLutThumbnails();
	HRESULT EnsureSvdLutPicture(DXGI_FORMAT format, UINT width, UINT height);
	bool SvdLutWorkAllowed();
	void BeginSvdLutScene(REFERENCE_TIME timestamp);
	void TakeSvdLutResult();
	void PollSvdLut();
	void CaptureSvdLut(REFERENCE_TIME timestamp, bool projected, bool second);
	const SvdLutScene* SvdLutSceneForPts(REFERENCE_TIME timestamp);
	struct SvdLutGrade {
		ID3D11PixelShader* shader;
		ID3D11ShaderResourceView* view;
		float blend;
		};
	std::optional<SvdLutGrade> PrepareSvdLutGrade(REFERENCE_TIME timestamp);
	HRESULT ApplySvdLutGrade(const SvdLutGrade& grade, const Tex2D_t& source, ID3D11Texture2D* target,
		const CRect& area, const CRect& visible, CPoint sourceOffset);
	HRESULT DrawCompletedSvdLut(const CompletedFrame& frame, ID3D11Texture2D* target,
		const CRect& picture, bool geometryChanging);
	void DrawSvdLut(ID3D11Texture2D* target, const CRect& picture, REFERENCE_TIME timestamp);

	// Independent unprotected, one-DirectML-evaluation-per-frame experiment B.
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
	SvdLutRawFrame* EvaluateSvdLutRawFrame(const Tex2D_t& source,
		const CRect& sample, REFERENCE_TIME pts, uint64_t fence);
	ID3D11ShaderResourceView* SvdLutRawViewForFrame(SvdLutRawFrame& frame);
	void PrefetchSvdLutRawFuture(const CompletedFrame& current, bool geometryChanging);
	HRESULT ApplySvdLutRawFrame(const Tex2D_t& source, ID3D11Texture2D* target,
		const CRect& area, const CRect& visible, CPoint offset, REFERENCE_TIME pts, uint64_t fence,
		bool allowTemporalCapture = true);
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
