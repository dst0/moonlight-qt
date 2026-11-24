// Runs every registered microphone test object; exit code = number of failed objects.
#include "testutil.h"

#include <QCoreApplication>
#include <QtTest>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Moonlight Game Streaming Project (mic tests)");
    QCoreApplication::setApplicationName("moonlight-mic-tests");
    micTestInitCommonC();

    int failed = 0;
    for (QObject* test : micTestObjects()) {
        failed += QTest::qExec(test, argc, argv) != 0 ? 1 : 0;
        delete test;
    }
    return failed;
}
