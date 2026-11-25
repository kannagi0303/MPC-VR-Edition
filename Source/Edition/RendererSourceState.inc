// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CMpcVideoRenderer source delivery state. Keep declaration order; this fragment is not a standalone header.
	// The film the source reads, for the logo library: read when the input pin connects,
	// and never asked of the graph again (CompleteConnect).
	std::wstring m_strFilm;
	std::optional<jvr::MediaProfile> m_jvrMediaProfile;
	bool JvrProjectionRequested(const Settings_t& settings) const;
	std::unique_ptr<class DecodedFrameLookahead> m_depthLookahead;
	bool m_localFile = false;
	std::unique_ptr<mfg::AvcPresentationClock> m_avcSourceClock;
	unsigned m_correctedSourcePts = 0;
	// Edition lifecycle hooks preserve caller lock scopes; see notes/upstream-integration.md.
	std::optional<HRESULT> GetEditionBool(LPCSTR field, bool* value);
	std::optional<HRESULT> GetEditionInt(LPCSTR field, int* value);
	std::optional<HRESULT> SetEditionBool(LPCSTR field, bool value);
	std::optional<HRESULT> SetEditionInt(LPCSTR field, int value);
	// Cancellation can be signalled before entering DirectShow/Renderer locks.
	mpcvr::edition::PlaybackCancellation m_playbackCancel;
	void CancelEditionDelivery() noexcept;
	void ResumeEditionDelivery(bool beginPlayback = false);
	void BeginEditionSegment(double rate);
	void BeginEditionFlush();
	void BeginEditionRun();
	void CompleteEditionRun(HRESULT hr);
	FILTER_STATE BeginEditionPause();
	void CompleteEditionPause(HRESULT hr, FILTER_STATE previous);
	void BeginEditionStop();
	HRESULT ReceiveEditionSample(IMediaSample* sample);
	HRESULT ReceiveWithSourceTime(IMediaSample* sample);
	void ResetSourceTime();
	double m_completedSegmentRate = 1.0;
	bool m_schedulingCompletedSample = false;
	uint64_t m_completedEpoch = 0;
	// [MFG-B11 | 2026-11-25 09:55 +08:00] Receive owns InterfaceLock
	// while publishing this renderer-lock waiter. Presenter reads it only
	// to schedule a bounded, lock-free handoff after a measured slow Present.
	std::atomic<bool> m_completedSourceWaiting{false};
	std::optional<HRESULT> ReceiveCompletedSample(IMediaSample* sample);
	std::atomic<bool> m_completedEndOfStream{false};
	HRESULT EndOfStream() override;
	bool TryCompleteBufferedEndOfStream();
	void OnReceiveMediaTime(IMediaSample* sample) override;
	void ReportCompletedMediaTime(REFERENCE_TIME pts, REFERENCE_TIME end);
	BOOL ScheduleSample(IMediaSample* sample) override;
	HRESULT ReceiveScheduled(IMediaSample* sample, uint64_t epoch);
	HRESULT WaitForRenderTime(uint64_t epoch);
	HRESULT WaitForRenderTime(uint64_t epoch, const mpcvr::edition::PlaybackCancellation::Ticket& ticket);
	bool LookaheadCancelled(uint64_t epoch) const;
	bool PrefetchZipDepth(IMediaSample* sample, int64_t pts, uint64_t epoch);
	void DrainLookahead();
	void ReportLookaheadError(HRESULT hr);
	std::optional<LRESULT> HandleEditionWindowMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void NormalizeEditionSettings();
	void OnEditionSettingsConfigured();
	void ShutdownEditionSource();
	void ReleaseEditionUiState();
