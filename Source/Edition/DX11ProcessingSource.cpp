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
#include "DX11VideoProcessor.h"
#include "VideoRenderer.h"

// Resolves only Edition sources. nullptr leaves native VP/color conversion in charge.
// Returned surfaces are borrowed; this call does not transfer GPU ownership.
const Tex2D_t* CDX11VideoProcessor::SelectEditionProcessingSource(const CRect& srcRect,
	const CRect& dstRect, bool second, bool allowDlssSR, REFERENCE_TIME timestamp,
	bool* overlayInputHandled, const Tex2D_t* preparedSource, CRect& processingSourceRect)
{
	CRect& rSrc = processingSourceRect;
	const bool srWasActive = m_dlssSR.Active();
	auto* srOutput = allowDlssSR ? TryDlssSR(srcRect, dstRect, second, timestamp, overlayInputHandled, preparedSource) : nullptr;
	if (allowDlssSR && !srOutput && srWasActive) {
		m_dlssSR.ResetHistory();
	}
	if (allowDlssSR && srWasActive != bool(srOutput) && m_mfgMultiplier && !MfgSourceMeetsDisplayRate()) {
		if (m_pFilter->m_Sets.bWaitForExtendedStartup) {
			StopMfg(false);
		} else {
			// Completed originals still belong to the previous visual route.
			// Keep their deadlines and restart interpolation at this boundary.
			ResetMfgFrameAnalysis();
			m_mfgRepeatTiming.ClearEvidence();
		}
		m_mfg.ResetHistory();
	}
	if (srOutput) {
		rSrc.SetRect(0, 0, srOutput->desc.Width, srOutput->desc.Height);
	}
	else if (preparedSource) {
		m_bVPScalingUseShaders = m_D3D11VP.IsReady();
	}
	return srOutput ? srOutput : preparedSource;
}
