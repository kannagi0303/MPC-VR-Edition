// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Dependencies for the Edition class fragments; imported once by the renderer header.
#include "../JvrProjection.h"
#include "StatisticsFormat.h"
#include "VisualStatistics.h"
#include "FeatureOsd.h"
#include "AdaptiveToneShot.h"

namespace edition { struct VideoFrame; }

#include <array>
#include <atomic>
#include <mutex>
#include "Mfg/Session.h"
#include "DLSS/DlssSR.h"
#include "Depth/Depth.h"
#include "DLSS/SvdLutModel.h"
#include "SvdLutTemporalStatistics.h"
#include "Mfg/FrameQueue.h"
#include "Mfg/LiveFlowGroups.h"
#include "Mfg/LiveFlowGroupPacing.h"
#include "Mfg/AdaptiveLookahead.h"
#include "Mfg/DisplayCadence.h"
#include "Mfg/DisplaySubmitPacing.h"
#include "Mfg/DisplayPreparation.h"
#include "Mfg/AutoCadence.h"
#include "Mfg/RepeatFrameTiming.h"
#include "Mfg/DecodedFrameDifference.h"
#include "Mfg/PresentationThread.h"


