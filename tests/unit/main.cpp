#include <QCoreApplication>
#include <QtGlobal>

#include "tests.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    int failures = 0;
    failures += runLanPolicyTests(argc, argv) != 0;
    failures += runKeyMappingTests(argc, argv) != 0;
    failures += runCertPinningTests(argc, argv) != 0;
    failures += runSourcePolicyTests(argc, argv) != 0;
#ifdef Q_OS_DARWIN
    failures += runKeychainTests(argc, argv) != 0;
    failures += runMetalYCoCgRTests(argc, argv) != 0;
#endif
    failures += runNvHttpTests(argc, argv) != 0;
    failures += runBoxArtTests(argc, argv) != 0;
    failures += runPreferencesTests(argc, argv) != 0;
    failures += runPacked444Tests(argc, argv) != 0;

    qInfo("%s: %d test group(s) failed", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
