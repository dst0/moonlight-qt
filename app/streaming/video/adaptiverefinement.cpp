#include "adaptiverefinement.h"

AdaptiveRefinementDecision resolveAdaptiveRefinementDecision(const AdaptiveRefinementInputs& in)
{
    AdaptiveRefinementDecision decision;
    if (!in.hostToggleEnabled) {
        return decision;
    }

    if (!in.hostSupportsTileRefinement) {
        decision.fallbackReason = QStringLiteral("host_missing_adaptive_tile_refinement");
        decision.statusNote = QStringLiteral("Connected host does not advertise adaptive_tile_refinement. Standard video streaming will be used.");
        return decision;
    }

    if (in.enableHdr) {
        decision.fallbackReason = QStringLiteral("hdr_conflict");
        decision.statusNote = QStringLiteral("Adaptive dirty-tile lossless refinement cannot be combined with HDR. Disable HDR to use adaptive tile refinement.");
        return decision;
    }

    const bool rendererAllowsMetal =
        in.rendererSelection == StreamingPreferences::RS_AUTO ||
        in.rendererSelection == StreamingPreferences::RS_METAL;
    if (!in.isMacOS || !in.isMetalAvailable || !rendererAllowsMetal) {
        decision.fallbackReason = QStringLiteral("non_metal_renderer");
        decision.statusNote = QStringLiteral("Adaptive dirty-tile lossless refinement requires the macOS Metal renderer (Auto or Metal).");
        return decision;
    }

    decision.active = true;
    decision.suppressPackedYCoCgR = true;
    decision.suppressNativeYuv444 = true;
    decision.forceMetalRenderer = true;
    if (in.enableYUV444) {
        decision.statusNote = QStringLiteral("Adaptive dirty-tile lossless refinement overrides 4:4:4 to stream fast 4:2:0 video with lossless RGB refinement on settled 64x64 text/UI tiles.");
    }
    return decision;
}
