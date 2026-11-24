#pragma once

#include <QObject>

class TestMacMicConverter : public QObject
{
    Q_OBJECT

private slots:
    void convertsDiscreteOrExpandedInputToMono_data();
    void convertsDiscreteOrExpandedInputToMono();
    void preservesOrdinaryConversion_data();
    void preservesOrdinaryConversion();
};
