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
#include "DLSS/ZipDepthLookahead.h"
#include "Mfg/AvcPresentationClock.h"

HRESULT CMpcVideoRenderer::CompleteConnect(IPin* pReceivePin)
{
	m_strFilm.clear();
	m_localFile = false;
	m_avcSourceClock.reset();
	m_correctedSourcePts = 0;
	CComPtr<IEnumFilters> filters;
	if (m_pGraph && SUCCEEDED(m_pGraph->EnumFilters(&filters))) {
		CComPtr<IBaseFilter> filter;
		while (filters->Next(1, &filter, nullptr) == S_OK) {
			CComQIPtr<IFileSourceFilter> source(filter);
			LPOLESTR path = nullptr;
			if (source && SUCCEEDED(source->GetCurFile(&path, nullptr)) && path) {
				const std::wstring name(path);
				CoTaskMemFree(path);
				m_strFilm = name.substr(name.find_last_of(L"\\/") + 1);
				const auto attributes = GetFileAttributesW(name.c_str());
				m_localFile = name.size() >= 3 && name[1] == L':' && GetDriveTypeW(name.substr(0, 3).c_str()) == DRIVE_FIXED &&
					attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
				if (m_localFile) {
					try {
						auto timing = std::make_unique<mfg::AvcPresentationClock>();
						timing->Load(name.c_str());
						if (timing->Available()) m_avcSourceClock = std::move(timing);
					} catch (const std::bad_alloc&) {
						DLog(L"AVC presentation timing inspection unavailable");
					}
				}
				break;
			}
			filter.Release();
		}
	}
	if (m_localFile && !m_depthLookahead && zipdepth::LookaheadMs()) {
		try {
			m_depthLookahead = std::make_unique<DecodedFrameLookahead>(*this, zipdepth::LookaheadMs());
		} catch (...) {
			DLog(L"ZipDepth lookahead worker unavailable");
		}
	}
	const HRESULT hr = __super::CompleteConnect(pReceivePin);
	if (SUCCEEDED(hr)) {
		auto profile = jvr::FindMediaProfile(g_hInst, m_strFilm);
		CAutoLock render(&m_RendererLock);
		m_jvrMediaProfile = std::move(profile);
		if (auto* dx11 = dynamic_cast<CDX11VideoProcessor*>(m_VideoProcessor.get());
			dx11 && dx11->m_jvrFlatView != JvrProjectionRequested(m_Sets)) {
			dx11->Configure(m_Sets);
		}
	}
	return hr;
}

HRESULT CMpcVideoRenderer::BreakConnect()
{
	const HRESULT hr = __super::BreakConnect();
	if (SUCCEEDED(hr)) {
		CAutoLock render(&m_RendererLock);
		m_jvrMediaProfile.reset();
	}
	return hr;
}

bool CMpcVideoRenderer::JvrProjectionRequested(const Settings_t& settings) const
{
	return settings.bUseD3D11 && settings.bJvrFlatView && m_jvrMediaProfile &&
		m_jvrMediaProfile->type == jvr::ProjectionType::JvrSbs;
}

