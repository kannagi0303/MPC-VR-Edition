// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <strmif.h>
#include <atltypes.h>

namespace edition {
// Borrowed inputs for one video-processing stage; no GPU resources or persistent state.
struct VideoFrame {
    int field;
    REFERENCE_TIME frameStartTime;
    bool mfgEnabledForRate;
    bool detached;
    bool completedVideoOnly;
    bool second;
    bool wantsProjection;
    bool projected;
    bool sourceReady;
    CRect frameSource;
    CRect outputPicture;
    CRect processingRect;
};
}
