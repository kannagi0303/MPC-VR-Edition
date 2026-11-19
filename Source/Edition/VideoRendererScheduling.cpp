/*
 * (C) 2018-2026 see Authors.txt
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
#include "Helper.h"
#include "VideoRenderer.h"
#include "DLSS/DecodedFrameLookahead.h"
#include "Mfg/AvcPresentationClock.h"

// A note on the thread itself, for whoever looks at a frozen player from outside.
//
// Windows keeps a description per thread and hands it out across processes, so a tool
// that walks the stacks of a hung player can read it with no cooperation and no file
// to leave behind. It is the only way to tell two waits apart that look identical in
// a stack. Resolved at the first call because Windows 7 does not have it.
static void SayWhatThisThreadWaitsFor(const std::wstring& what)
{
	using PFN_SetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static PFN_SetThreadDescription pfn = []() -> PFN_SetThreadDescription {
		const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		return kernel ? (PFN_SetThreadDescription)GetProcAddress(kernel, "SetThreadDescription") : nullptr;
	}();
	if (pfn) {
		pfn(GetCurrentThread(), what.c_str());
	}
}

// Who gives the graph its clock. The renderer schedules every picture against it, so
// when it stops the first thing worth knowing is whose it is -- and a reference clock
// that comes from the sound card stops when the sound does.
void CMpcVideoRenderer::NoteWhoKeepsTime()
{
	wcscpy_s(m_szClockName, L"none");
	if (!m_pClock) {
		return;
	}
	wcscpy_s(m_szClockName, L"the system");
	if (CComQIPtr<IBaseFilter> pClockFilter = m_pClock) {
		FILTER_INFO info = {};
		if (SUCCEEDED(pClockFilter->QueryFilterInfo(&info))) {
			if (info.achName[0]) {
				wcsncpy_s(m_szClockName, info.achName, _TRUNCATE);
			}
			if (info.pGraph) {
				info.pGraph->Release();
			}
		}
	}
}

// Waiting for this picture's turn, with a way out.
//
// A picture is given its turn in PrepareReceive: the clock is asked to signal an
// event when its time comes, and the streaming thread then waits for that event
// here. Pause cancels that advise and resets the event -- and it does so whether or
// not the event has already been signalled, so a turn that came at the wrong moment
// is simply lost. Play is supposed to give the picture a new turn, and usually does.
// When it does not, the wait upstream waits ten seconds, finds nothing, and waits ten
// seconds again, for ever: the graph goes on running, the sound with it, and the
// picture never moves again until the player is paused and played once more. That is
// the freeze reported on a display at 24 or 25 Hz, where the window between a
// picture being scheduled and this wait being entered is a whole refresh wide.
//
// So the wait is bounded, and what it finds at the bound decides. Two things can go
// wrong and they are counted apart, because which one fires says what happened.
//
//   no advise left   the turn was taken away after it was given, and nothing will
//                    ever wake this thread. Take the picture now rather than never.
//   advise still set but seconds have passed. A renderer is handed one picture at a
//                    time and waits at most a frame or two for it, so a turn that is
//                    still seconds away means the time it was scheduled against is
//                    not the time the clock is keeping. Take it too.
//
// A filter that really is paused is left alone: waiting there is right, and Play will
// hand out the turn.
//
// While a wait goes long the thread says what it is waiting for in its own
// description, which a dump of the frozen player reads straight out: the two cases
// are indistinguishable from the outside otherwise, and both look like a thread
// sitting in WaitForRenderTime.
HRESULT CMpcVideoRenderer::WaitForRenderTime()
{
	return WaitForRenderTime(0);
}

HRESULT CMpcVideoRenderer::WaitForRenderTime(uint64_t epoch)
{
	HANDLE objects[2] = { m_ThreadSignal, m_RenderEvent };
	const ULONGLONG waitStart = GetTickCount64();
	bool bSaidSo = false;
	// Whatever way this is left, the note goes with it: a dump that reads a note
	// from a wait that has already ended points at the wrong thing entirely.
	const auto Leave = [&bSaidSo](HRESULT hr) {
		if (bSaidSo) {
			SayWhatThisThreadWaitsFor(L"MPCVR streaming");
		}
		return hr;
	};

	// A stopped clock cannot be waited for, so while it is stopped this is where the
	// time comes from: each picture is due one frame duration after the one before
	// it, counted from when that one was taken and not from now, so that the time
	// spent drawing and presenting is part of the frame rather than added to it.
	// That is the difference between the film's own rate and a third of it.
	const ULONGLONG frameMs = std::max<ULONGLONG>(10, m_FrameStats.GetAverageFrameDuration() / 10000);
	if (m_bClockStalled && !m_tickPictureDue) {
		m_tickPictureDue = GetTickCount64() + frameMs;
	}

	for (;;) {
		if (LookaheadCancelled(epoch)) return Leave(VFW_E_STATE_CHANGED);
		DWORD quantum = 500;
		if (m_bClockStalled) {
			const ULONGLONG now = GetTickCount64();
			quantum = (DWORD)std::clamp<ULONGLONG>(m_tickPictureDue > now ? m_tickPictureDue - now : 0, 1, 500);
		}
		const DWORD result = WaitForMultipleObjects(2, objects, FALSE, quantum);
		if (result == WAIT_OBJECT_0) {
			return Leave(VFW_E_STATE_CHANGED);
		}
		if (result == WAIT_OBJECT_0 + 1) {
			SignalTimerFired();
			// A turn that arrives is a clock that runs.
			m_bClockStalled = false;
			m_tickPictureDue = 0;
			return Leave(NOERROR);
		}
		if (result != WAIT_TIMEOUT) {
			return Leave(VFW_E_STATE_CHANGED);
		}
		if (!m_bStreaming && m_State != State_Running) {
			continue;   // paused for real
		}

		if (m_bClockStalled) {
			const ULONGLONG now = GetTickCount64();
			if (now < m_tickPictureDue) {
				continue;
			}
			// The next one is due a frame later. If this one has fallen a long way
			// behind -- the machine was busy, or the stall is only starting -- the
			// count starts again from here rather than chasing a debt it cannot pay.
			m_tickPictureDue = (now - m_tickPictureDue > 4 * frameMs)
				? now + frameMs : m_tickPictureDue + frameMs;
			m_nRenderWaitsLate++;
			return Leave(NOERROR);
		}

		const ULONGLONG bound = 1000;
		const ULONGLONG waited = GetTickCount64() - waitStart;
		if (!bSaidSo) {
			// Half a second of waiting never happens while a film plays, so this
			// costs nothing in the ordinary case.
			bSaidSo = true;
			SayWhatThisThreadWaitsFor(std::format(
				L"MPCVR waiting for a picture's turn: advise {}, streaming {}, state {}",
				m_dwAdvise ? L"live" : L"gone", m_bStreaming ? 1 : 0, (int)m_State));
		}

		if (!m_dwAdvise) {
			m_nRenderWaitsBroken++;
			DLog(L"CMpcVideoRenderer::WaitForRenderTime() : the turn was taken away, taking the picture now ({} so far)", m_nRenderWaitsBroken);
			return Leave(NOERROR);
		}
		if (waited >= bound) {
			m_nRenderWaitsLate++;
			// How far away the turn was, and how far the clock moved since the last
			// time one was this far away. A renderer and the pictures it is fed keep
			// the same time or they do not: if the distance stays the same picture
			// after picture while the clock runs normally, the two are simply not
			// measuring from the same place, and no amount of waiting will bring them
			// together. These two numbers are the difference between knowing that and
			// guessing it, and they cost nothing until something has already gone
			// wrong.
			// Read out of values, never out of the pending sample: this thread does
			// not hold the renderer lock while it waits, and the sample can be let go
			// from under it by a flush. Reading it here cost an access violation in
			// the bench before this was written down.
			CRefTime rtStream;
			if (SUCCEEDED(StreamTime(rtStream))) {
				const REFERENCE_TIME rtBase = (REFERENCE_TIME)m_tStart;
				const REFERENCE_TIME rtClockNow = (REFERENCE_TIME)rtStream + rtBase;
				const REFERENCE_TIME rtSample = m_FrameStats.GeTimestamp();
				// The three are kept apart on purpose. The distance to the turn says
				// how far out of step the two are; the clock says whether time is
				// passing at all; and the base is the only thing that moves between
				// them, so a distance that holds while the clock runs is the base.
				m_msLastTurnAhead = (int)((rtSample - (REFERENCE_TIME)rtStream) / 10000);
				m_msClockSinceLastTurn = m_rtLastTurnClock ? (int)((rtClockNow - m_rtLastTurnClock) / 10000) : 0;
				m_msBaseSinceLastTurn = m_rtLastTurnBase ? (int)((rtBase - m_rtLastTurnBase) / 10000) : 0;
				// Less than a millisecond of clock between two turns that were a
				// bound apart is a clock that has stopped, whatever it says.
				if (m_rtLastTurnClock && std::abs(m_msClockSinceLastTurn) < 1) {
					m_bClockStalled = true;
				}
				m_rtLastTurnClock = rtClockNow;
				m_rtLastTurnBase = rtBase;
			}
			DLog(L"CMpcVideoRenderer::WaitForRenderTime() : {} ms waiting on a turn still {} ms away, taking the picture now ({} so far)",
				waited, m_msLastTurnAhead, m_nRenderWaitsLate);
			return Leave(NOERROR);
		}
	}
}

HRESULT CMpcVideoRenderer::ReceiveEditionSample(IMediaSample* pSample)
{
	if (!m_depthLookahead) {
		return ReceiveWithSourceTime(pSample);
	}
	if (!m_depthLookahead || !m_localFile || m_bFlushing) return ReceiveScheduled(pSample, 0);
	const auto epoch = m_depthLookahead->Epoch();
	CComPtr<IMediaSample> detached;
	bool projected = false;
	int64_t pts = -1;
	uint64_t bytes = 0;
	{
		CAutoLock lock(&m_RendererLock);
		if (m_filterState == State_Running && !m_bFlushing) {
			if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) {
				projected = dx11->CanProjectJvr();
				if (!projected) detached = dx11->CloneLookaheadSample(pSample, pts, bytes);
			}
		}
	}
	if (detached) {
		try {
			m_depthLookahead->Push(detached, pts, bytes, epoch);
			return S_OK;
		} catch (const std::bad_alloc&) {
			detached.Release();
		}
	}
	DrainLookahead();
	if (LookaheadCancelled(epoch)) return S_OK;
	{
		CAutoLock lock(&m_RendererLock);
		if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get()))
			dx11->StopZipDepthLookahead();
	}
	return projected ? ReceiveWithSourceTime(pSample) : ReceiveScheduled(pSample, 0);
}

void CMpcVideoRenderer::ResetSourceTime()
{
	if (m_avcSourceClock) m_avcSourceClock->Reset();
	m_correctedSourcePts = 0;
}

HRESULT CMpcVideoRenderer::ReceiveWithSourceTime(IMediaSample* sample)
{
	struct RestoreTime {
		IMediaSample* sample;
		REFERENCE_TIME start = 0, end = 0;
		bool changed = false;
		~RestoreTime() { if (changed) sample->SetTime(&start, &end); }
	} original{sample};
	if (m_avcSourceClock) {
		CAutoLock lock(&m_RendererLock);
		const auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
		if (!m_bFlushing && m_filterState != State_Stopped && m_completedSegmentRate == 1.0 &&
			m_Sets.iMfgMultiplier && dx11 && !dx11->m_bInterlaced && m_inputMT.subtype == MEDIASUBTYPE_NV12 &&
			sample->IsPreroll() != S_OK && sample->GetTime(&original.start, &original.end) == S_OK) {
			AM_MEDIA_TYPE* changed = nullptr;
			const bool changesFormat = sample->GetMediaType(&changed) == S_OK;
			if (changed) DeleteMediaType(changed);
			if (changesFormat) {
				ResetSourceTime();
			} else if (auto time = m_avcSourceClock->Next(original.start, original.end, sample->IsDiscontinuity() == S_OK);
				time && (time->first != original.start || time->second != original.end)) {
				original.changed = SUCCEEDED(sample->SetTime(&time->first, &time->second));
				if (original.changed) {
					if (time->first != original.start) ++m_correctedSourcePts;
				} else {
					ResetSourceTime();
				}
			}
		} else if (!m_Sets.iMfgMultiplier) {
			ResetSourceTime();
		}
	}
	if (const auto hr = ReceiveCompletedSample(sample)) return *hr;
	return ReceiveScheduled(sample, 0);
}

BOOL CMpcVideoRenderer::ScheduleSample(IMediaSample* sample)
{
	// Both scheduling paths hold RendererLock. Observe input PTS before the
	// scheduling decision, even when the renderer will skip drawing this sample.
	REFERENCE_TIME start, stop;
	const auto timing = sample ? sample->GetTime(&start, &stop) : E_POINTER;
	if (SUCCEEDED(timing) && sample->IsPreroll() != S_OK) {
		if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get())) {
			dx11->m_mfgDisplayRate.Observe(start, dx11->m_rtAvgTimePerFrame, dx11->m_dRefreshPeriodMs,
				dx11->m_bDoubleFrames, sample->IsDiscontinuity() == S_OK);
		}
	}
	if (!m_schedulingCompletedSample) return CBaseVideoRenderer2::ScheduleSample(sample);
	if (timing == S_OK) m_FrameStats.Add(start);
	return TRUE;
}

std::optional<HRESULT> CMpcVideoRenderer::ReceiveCompletedSample(IMediaSample* sample)
{
	if (!m_localFile) return {};
	REFERENCE_TIME pts = 0, end = 0;
	AM_MEDIA_TYPE* changed = nullptr;
	const bool changesFormat = sample->GetMediaType(&changed) == S_OK;
	if (changed) DeleteMediaType(changed);
	const bool valid = !changesFormat && sample->IsPreroll() != S_OK &&
		sample->GetTime(&pts, &end) == S_OK && pts >= 0 && end > pts;
	const bool discontinuity = sample->IsDiscontinuity() == S_OK;
	std::optional<uint64_t> epoch;
	std::optional<uint64_t> deferredFrameFence;
	for (;;) {
		{
			CAutoLock state(&m_InterfaceLock);
			CAutoLock render(&m_RendererLock);
			if (!epoch) epoch = m_completedEpoch;
			if (m_bFlushing || m_State == State_Stopped || *epoch != m_completedEpoch) return S_OK;
			auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
			if (!dx11) return {};
			if (dx11->m_completedPresenter.Failed()) dx11->ClearCompletedFrames(true);
			const bool pending = dx11->CompletedFramesPending() && !dx11->m_completedPresenter.Failed();
			if (m_State != State_Running || !m_bStreaming) {
				deferredFrameFence.reset();
				if (!pending) return {};
			} else if (!valid || m_completedSegmentRate != 1.0 || !dx11->CanBufferCompletedSample()) {
				if (!pending) return {};
			} else if ((!discontinuity || !pending) && !dx11->LegacyFramesPending() &&
			           dx11->CanProduceCompletedFrame(pts, deferredFrameFence)) {
				if (dx11->PrepareCompletedTarget()) {
					m_schedulingCompletedSample = true;
					const auto prepared = PrepareReceive(sample);
					m_schedulingCompletedSample = false;
					if (FAILED(prepared)) {
						dx11->m_completedTarget.reset();
						return prepared == VFW_E_SAMPLE_REJECTED ? S_OK : prepared;
					}
					m_bInReceive = FALSE;
					const auto hr = dx11->RenderCompletedSample(sample);
					ClearPendingSample();
					CancelNotification();
					return hr;
				}
				if (!pending) return {};
			}
		}
		// Capacity and timeline waits must allow the presenter and BeginFlush through.
		Sleep(2);
	}
}

HRESULT CMpcVideoRenderer::EndOfStream()
{
	auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
	if (dx11 && dx11->CompletedFramesPending() && !dx11->m_completedPresenter.Failed()) {
		m_completedEndOfStream = true;
		dx11->m_completedPresenter.Wake();
		return S_OK;
	}
	return __super::EndOfStream();
}

bool CMpcVideoRenderer::TryCompleteBufferedEndOfStream()
{
	if (!m_completedEndOfStream || !m_InterfaceLock.TryLock()) return false;
	struct Unlock { CCritSec& lock; ~Unlock() { lock.Unlock(); } } state{m_InterfaceLock};
	if (!m_RendererLock.TryLock()) return false;
	Unlock render{m_RendererLock};
	auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
	if (!m_completedEndOfStream || m_bFlushing || m_State != State_Running ||
	    (dx11 && dx11->CompletedFramesPending())) return false;
	m_completedEndOfStream = false;
	__super::EndOfStream();
	return true;
}

void CMpcVideoRenderer::OnReceiveMediaTime(IMediaSample* sample)
{
	if (!m_schedulingCompletedSample) __super::OnReceiveMediaTime(sample);
}

void CMpcVideoRenderer::ReportCompletedMediaTime(REFERENCE_TIME pts, REFERENCE_TIME end)
{
	if (m_pPosition) m_pPosition->RegisterMediaTime(pts, end);
}

bool CMpcVideoRenderer::LookaheadCancelled(uint64_t epoch) const
{
	return epoch && m_depthLookahead->Epoch() != epoch;
}

bool CMpcVideoRenderer::PrefetchZipDepth(IMediaSample* sample, int64_t pts, uint64_t epoch)
{
	CAutoLock lock(&m_RendererLock);
	if (LookaheadCancelled(epoch) || m_bFlushing || m_filterState == State_Stopped) return true;
	if (m_filterState == State_Paused) return false;
	auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
	return !dx11 || dx11->PrefetchZipDepth(sample, pts);
}

void CMpcVideoRenderer::DrainLookahead()
{
	if (m_depthLookahead) m_depthLookahead->Drain();
}

void CMpcVideoRenderer::ReportLookaheadError(HRESULT hr)
{
	CAutoLock lock(&m_InterfaceLock);
	if (m_bFlushing || m_filterState == State_Stopped) return;
	m_depthLookahead->Cancel();
	NotifyEvent(EC_ERRORABORT, hr, 0);
}

HRESULT CMpcVideoRenderer::ReceiveScheduled(IMediaSample* pSample, uint64_t epoch)
{
	// override CBaseRenderer::Receive() for the implementation of the search during the pause

	if (m_bFlushing || LookaheadCancelled(epoch)) {
		DLog(L"CMpcVideoRenderer::Receive() - flushing, skip sample");
		return S_OK;
	}

	ASSERT(pSample);

	// It may return VFW_E_SAMPLE_REJECTED code to say don't bother

	HRESULT hr = PrepareReceive(pSample);
	ASSERT(m_bInReceive == SUCCEEDED(hr));
	if (FAILED(hr)) {
		if (hr == VFW_E_SAMPLE_REJECTED) {
			return NOERROR;
		}
		return hr;
	}

	// PrepareReceive has stored the first sample, but the graph has not yet
	// been told that this renderer is ready. Complete the paused poster draw
	// (including lazy DLSS SR and MFG NGX initialization) BEFORE Ready().
	// Otherwise MPC-BE can start its audio clock while these expensive GPU
	// resources are still being created on the first running video sample.
	if (m_State == State_Paused) {
		m_bInReceive = FALSE;
		{
			CAutoLock cRendererLock(&m_RendererLock);
			DoRenderSample(m_pMediaSample);
		}
		{
			// Keep the original CBaseRenderer handshake and lock ordering:
			// never hold RendererLock while acquiring InterfaceLock.
			CAutoLock cVideoLock(&m_InterfaceLock);
			if (m_State == State_Stopped) return NOERROR;
			m_bInReceive = TRUE;
			Ready();
		}
	}

	// Having set an advise link with the clock we sit and wait. We may be
	// awoken by the clock firing or by a state change. The rendering call
	// will lock the critical section and check we can still render the data

	hr = WaitForRenderTime(epoch);
	if (FAILED(hr)) {
		m_bInReceive = FALSE;
		if (LookaheadCancelled(epoch)) {
			CAutoLock cVideoLock(&m_InterfaceLock);
			CAutoLock cRendererLock(&m_RendererLock);
			if (m_pMediaSample == pSample) {
				ClearPendingSample();
				CancelNotification();
			}
		}
		return NOERROR;
	}

	//  Set this here and poll it until we work out the locking correctly
	//  It can't be right that the streaming stuff grabs the interface
	//  lock - after all we want to be able to wait for this stuff
	//  to complete
	m_bInReceive = FALSE;

	for (;;) {
		{
			CAutoLock cVideoLock(&m_InterfaceLock);
			CAutoLock cRendererLock(&m_RendererLock);
			if (m_State == State_Stopped || m_pMediaSample != pSample) return NOERROR;

			const bool running = m_State == State_Running && !m_bFlushing && !LookaheadCancelled(epoch);
			auto* dx11 = running && !m_bClockStalled && m_Sets.iMfgMultiplier
				? dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get()) : nullptr;
			const bool drainMfg = dx11 && dx11->MfgSourceMeetsDisplayRate() && dx11->LegacyFramesPending();
			if (!drainMfg) {
				if (running) Render(m_pMediaSample);
				ClearPendingSample();
				SendEndOfStream();
				CancelNotification();
				return NOERROR;
			}
		}
		// Let the presenter finish the older originals on their own deadlines.
		Sleep(2);
	}
}

