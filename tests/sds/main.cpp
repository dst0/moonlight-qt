// secure-desktop-stream client tests. Set SDS_TEST_ONLY to a suite name (sanitizer,
// downloaddir, clipboard, controller, client) to run just that suite.

#include <QByteArray>
#include <QCoreApplication>

int runSanitizerTests(int argc, char** argv);
int runDownloadDirTests(int argc, char** argv);
int runClipboardTests(int argc, char** argv);
int runFileClipboardTests(int argc, char** argv);
int runControllerTests(int argc, char** argv);
int runSdsClientTests(int argc, char** argv);

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QByteArray only = qgetenv("SDS_TEST_ONLY");
    struct Suite
    {
        const char* name;
        int (*run)(int, char**);
    } suites[] = {
        { "sanitizer", runSanitizerTests },
        { "downloaddir", runDownloadDirTests },
        { "clipboard", runClipboardTests },
        { "fileclipboard", runFileClipboardTests },
        { "controller", runControllerTests },
        { "client", runSdsClientTests },
    };

    int failed = 0;
    for (const Suite& s : suites) {
        if (only.isEmpty() || only == s.name) {
            failed += s.run(argc, argv);
        }
    }
    return failed == 0 ? 0 : 1;
}
