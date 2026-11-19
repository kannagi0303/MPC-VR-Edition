/*
 * (C) 2020-2026 see Authors.txt
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
#include "IVideoRenderer.h"
#include "VideoProcessor.h"

bool CVideoProcessor::UpdateStatsLayout(const std::wstring& text)
{
	int lines = 1;
	int columns = 0;
	int column = 0;
	for (const wchar_t c : text) {
		if (c == L'\n') {
			lines++;
			columns = std::max(columns, column);
			column = 0;
		} else {
			column++;
		}
	}
	columns = std::clamp(std::max(columns, column), kStatsMinColumns, kStatsMaxColumns);

	if (lines > m_StatsLines || columns > m_StatsColumns) {
		m_StatsLines = std::max(lines, m_StatsLines);
		m_StatsColumns = std::max(columns, m_StatsColumns);
		m_StatsShrinkCount = 0;
		return true;
	}
	if (lines < m_StatsLines || columns < m_StatsColumns) {
		// About a second at any frame rate anyone plays films at.
		if (++m_StatsShrinkCount < 30) {
			return false;
		}
		m_StatsLines = lines;
		m_StatsColumns = columns;
		m_StatsShrinkCount = 0;
		return true;
	}
	m_StatsShrinkCount = 0;
	return false;
}

