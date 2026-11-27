#pragma once

#include <QObject>

class TestMetalRefinement : public QObject
{
    Q_OBJECT

private slots:
    void refinesStaticColoredTextTileBitExactOnBiplanarNv12();
    void zeroGhostingOnLumaShiftWithStaleRefinementTexture();
    void resetAllClearingRestoresVideoOnTriplanarI420();
    void computeValidationKeepsStaticTextWithEdgeLumaDriftAndRejectsMotion();
    void scaledOutputAppliesBilinearFilterWithoutNeighborBleed();
};
