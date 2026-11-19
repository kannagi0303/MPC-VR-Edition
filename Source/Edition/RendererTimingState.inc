// SPDX-License-Identifier: GPL-3.0-or-later
// Inside CMpcVideoRenderer timing state. Keep declaration order; this fragment is not a standalone header.
	// Startup is not specific to MFG: SR, DLAA, VSR and MFG may all be
	// preparing before the first picture is ready for the DirectShow clock.
	std::atomic<bool> m_bStartupOverlayPending{ false };
	std::atomic<unsigned> m_featureStatusPlaybackRun{ 0 };
	// How often a picture's turn was cancelled after it had been scheduled, leaving
	// the streaming thread with nothing that could ever wake it. Nothing should ever
	// raise this; the statistics carry it because that is the only place it can be
	// seen from, and because it is the proof that the freeze it prevents was real.
	int m_nRenderWaitsBroken = 0;
	// How often a picture's turn was still seconds away when the renderer had been
	// handed only that one picture: the time it was scheduled against is not the time
	// the clock is keeping. Counted apart from the one above because which of the two
	// fires is what says where the fault is.
	int m_nRenderWaitsLate = 0;
	// What the last overdue turn looked like: how far away it still was, and how far
	// the clock had moved since the one before it. A distance that stays the same
	// while the clock runs normally means the renderer and the pictures it is fed are
	// not measuring from the same place.
	int m_msLastTurnAhead = 0;
	int m_msClockSinceLastTurn = 0;
	int m_msBaseSinceLastTurn = 0;
	REFERENCE_TIME m_rtLastTurnClock = 0;
	REFERENCE_TIME m_rtLastTurnBase = 0;
	// Set when the clock was caught not moving between two turns given up on, and
	// cleared by the first turn that arrives. While it is set the pictures are paced
	// by the stream's own frame duration, because a stopped clock cannot be waited
	// for and every picture after one looks further ahead than the last.
	bool m_bClockStalled = false;
	// When the next picture is due while the clock is stopped, in ticks. Kept from
	// one picture to the next so the rate is the film's and not the film's minus
	// whatever drawing cost.
	ULONGLONG m_tickPictureDue = 0;
	// Which filter gives the graph its clock, for the statistics: when that clock
	// stops, the first thing worth knowing is whose it is. A fixed buffer because the
	// statistics are drawn on another thread than the one that fills it.
	wchar_t m_szClockName[64] = {};
	void NoteWhoKeepsTime();
