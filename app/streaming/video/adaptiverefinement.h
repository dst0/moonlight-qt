#pragma once

#include <QString>

#include "settings/streamingpreferences.h"

struct AdaptiveRefinementInputs
{
    bool hostToggleEnabled = false;
    bool hostSupportsTileRefinement = false;
    bool enableHdr = false;
    bool enableYUV444 = false;
    bool isMacOS = false;
    StreamingPreferences::RendererSelection rendererSelection = StreamingPreferences::RS_AUTO;
    bool isMetalAvailable = false;
};

struct AdaptiveRefinementDecision
{
    bool active = false;
    bool suppressPackedYCoCgR = false;
    bool suppressNativeYuv444 = false;
    bool forceMetalRenderer = false;
    QString statusNote;
    QString fallbackReason;
};

AdaptiveRefinementDecision resolveAdaptiveRefinementDecision(const AdaptiveRefinementInputs& in);
