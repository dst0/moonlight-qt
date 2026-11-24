#pragma once

#include <QObject>

class TestMetalYCoCgR : public QObject
{
    Q_OBJECT

private slots:
    void unpacks1080pExactAt1To1_VTMsbMode();
    void unpacks1080pExactAt1To1_SwLsbMode();
    void unpacksOddResolutionExactAt1To1();
    void unpacks1080pScaledTo720pWithoutSeamBleed();
};
