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
#include "resource.h"

bool CDX11VideoProcessor::PrepareEditionSubtitleShader()
{
	if (m_bPGSSubtitles && SourceIsHDR10()) {
		if (m_bHdrDisplayModeEnabled) {
			float SDR_peak_lum;
			switch (m_iHdrOsdBrightness) {
				default:
					SDR_peak_lum = 100;
					break;
				case 1:
					SDR_peak_lum = 50;
					break;
				case 2:
					SDR_peak_lum = 30;
					break;
			}

			m_pPS_BitmapToFrame = m_pPS_Simple;
			m_dwStatsTextColor = TransferPQ(D3DCOLOR_XRGB(255, 255, 255), SDR_peak_lum);
		} else {
			if (m_bConvertToSdr) {
				m_pPS_BitmapToFrame.Release();
				EXECUTE_ASSERT(S_OK == CreatePShaderFromResource(&m_pPS_BitmapToFrame, IDF_PS_11_CONVERT_PQ_TO_SDR));
			} else {
				m_pPS_BitmapToFrame = m_pPS_Simple;
			}
			m_dwStatsTextColor = D3DCOLOR_XRGB(255, 255, 255);
		}

		return true;
	}

	return false;
}

void CDX11VideoProcessor::SetPGSSubtitles(bool bPGSSubtitles) {
	if (bPGSSubtitles != m_bPGSSubtitles) {
		m_bPGSSubtitles = bPGSSubtitles;

		CAutoLock cRendererLock(&m_pFilter->m_RendererLock);
		UpdateBitmapShader();
	}
}
