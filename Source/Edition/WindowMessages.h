// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>

// The video-window thread owns the label. Streaming threads only post state
// changes and never block on UI painting while holding renderer locks.
inline constexpr UINT WM_VR_STARTUP_OVERLAY = WM_APP + 0x3A2;
inline constexpr UINT WM_VR_GEOMETRY = WM_APP + 0x3A3;
inline constexpr UINT_PTR VR_GEOMETRY_TIMER = 0x3A3;


inline constexpr UINT WM_VR_REMAKE = WM_APP + 0x3A1;
inline constexpr wchar_t RendererWindowClassName[] = L"VRWindow";
