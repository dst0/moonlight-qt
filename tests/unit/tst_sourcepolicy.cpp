// Static policy checks over the client sources: no references to the removed Internet
// services and no URL literals except an allowlist of user-clickable help links.

#include <QtTest>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

#include "tests.h"

namespace {

QString sourceRoot()
{
    const QString root = qEnvironmentVariable("SDS_SOURCE_ROOT");
    if (!root.isEmpty()) {
        return root;
    }
    const QString source = QFINDTESTDATA("tst_sourcepolicy.cpp");
    return source.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(source).absolutePath() + "/../..");
}

QString readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

// Removes // and /* */ comments from C, C++, Objective-C, QML and JavaScript source while
// keeping string and character literals intact. Newlines are preserved so line numbers stay valid.
QString stripComments(const QString& source)
{
    enum { Code, LineComment, BlockComment, DoubleQuoted, SingleQuoted, Template } state = Code;
    QString out;
    out.reserve(source.size());

    for (int i = 0; i < source.size(); i++) {
        QChar c = source[i];
        QChar next = i + 1 < source.size() ? source[i + 1] : QChar();

        switch (state) {
        case Code:
            if (c == '/' && next == '/') {
                state = LineComment;
                i++;
            }
            else if (c == '/' && next == '*') {
                state = BlockComment;
                i++;
            }
            else {
                if (c == '"') state = DoubleQuoted;
                else if (c == '\'') state = SingleQuoted;
                else if (c == '`') state = Template;
                out += c;
            }
            break;
        case LineComment:
            if (c == '\n') {
                state = Code;
                out += c;
            }
            break;
        case BlockComment:
            if (c == '*' && next == '/') {
                state = Code;
                i++;
            }
            else if (c == '\n') {
                out += c;
            }
            break;
        case DoubleQuoted:
        case SingleQuoted:
        case Template:
            out += c;
            if (c == '\\' && i + 1 < source.size()) {
                out += source[++i];
            }
            else if ((state == DoubleQuoted && c == '"') ||
                     (state == SingleQuoted && c == '\'') ||
                     (state == Template && c == '`')) {
                state = Code;
            }
            else if (c == '\n' && state != Template) {
                // Unterminated literal (e.g. an apostrophe in a preprocessor line)
                state = Code;
            }
            break;
        }
    }

    return out;
}

int lineOf(const QString& text, qsizetype offset)
{
    return text.left(offset).count('\n') + 1;
}

// All C/C++/Objective-C/QML/JS sources under app/ (excluding the gamepad DB submodule)
QStringList appSources()
{
    QStringList files;
    QDirIterator it(sourceRoot() + QStringLiteral("/app"),
                    {"*.c", "*.cpp", "*.h", "*.hpp", "*.m", "*.mm", "*.qml", "*.js"},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString path = it.next();
        if (!path.contains(QStringLiteral("/SDL_GameControllerDB/"))) {
            files.append(path);
        }
    }
    files.sort();
    return files;
}

// The moonlight-common-c sources the client's qmake project compiles, plus its headers
QStringList commonCSources(QStringList* listedSourcesOut = nullptr)
{
    const QString proDir = sourceRoot() + QStringLiteral("/moonlight-common-c");
    const QString commonDir = proDir + QStringLiteral("/moonlight-common-c");
    QString pro = readFile(proDir + QStringLiteral("/moonlight-common-c.pro"));

    QStringList files;
    QStringList listed;
    static const QRegularExpression entry(QStringLiteral("^\\s*\\$\\$(COMMON_C_DIR|ENET_DIR)/(\\S+\\.[ch])(?=\\s|$)"),
                                          QRegularExpression::MultilineOption);
    auto matches = entry.globalMatch(pro);
    while (matches.hasNext()) {
        auto match = matches.next();
        QString base = match.captured(1) == QStringLiteral("ENET_DIR") ? commonDir + QStringLiteral("/enet") : commonDir;
        listed.append(match.captured(2));
        files.append(base + QStringLiteral("/") + match.captured(2));
    }
    if (listedSourcesOut != nullptr) {
        *listedSourcesOut = listed;
    }

    for (const QString& dir : {commonDir + QStringLiteral("/src"), commonDir + QStringLiteral("/enet/include")}) {
        QDirIterator it(dir, {"*.h"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(it.next());
        }
    }

    files.removeDuplicates();
    files.sort();
    return files;
}

// Hosts and names of the removed Internet services. Matched case-insensitively against the
// full text, comments included.
const char* const kForbiddenPatterns[] = {
    "moonlight-stream\\.org",
    "conntest",
    "discord",
    "ipv4only\\.arpa",
    "\\bstun\\.[a-z0-9-]",   // stun.<host>, but not a sentence ending in "STUN."
};

// User-clickable help links (opened in the user's browser, never fetched by the client).
// They are only allowed in QML files.
const char* const kAllowedUrls[] = {
    "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide",
    "https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting",
    "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems",
    "https://github.com/moonlight-stream/moonlight-docs/wiki/Gamepad-Mapping",
    "https://github.com/moonlight-stream/moonlight-qt/releases",
};

QStringList findForbiddenReferences(const QStringList& files)
{
    QStringList problems;
    for (const char* pattern : kForbiddenPatterns) {
        QRegularExpression re(QString::fromLatin1(pattern), QRegularExpression::CaseInsensitiveOption);
        for (const QString& file : files) {
            QString text = readFile(file);
            auto matches = re.globalMatch(text);
            while (matches.hasNext()) {
                auto match = matches.next();
                problems.append(QStringLiteral("%1:%2: %3").arg(file).arg(lineOf(text, match.capturedStart())).arg(match.captured()));
            }
        }
    }
    return problems;
}

QStringList findUnexpectedUrls(const QStringList& files, bool allowHelpLinksInQml)
{
    static const QRegularExpression url(QStringLiteral("https?://[^\\s\"'`)<>]*"),
                                        QRegularExpression::CaseInsensitiveOption);
    QStringList allowed;
    for (const char* u : kAllowedUrls) {
        allowed.append(QString::fromLatin1(u));
    }

    QStringList problems;
    for (const QString& file : files) {
        QString code = stripComments(readFile(file));
        bool isQml = file.endsWith(QStringLiteral(".qml"));
        auto matches = url.globalMatch(code);
        while (matches.hasNext()) {
            auto match = matches.next();
            if (allowHelpLinksInQml && isQml && allowed.contains(match.captured())) {
                continue;
            }
            problems.append(QStringLiteral("%1:%2: %3").arg(file).arg(lineOf(code, match.capturedStart())).arg(match.captured()));
        }
    }
    return problems;
}

}

class TestSourcePolicy : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qInfo() << "Source root:" << sourceRoot();
        QVERIFY2(QFile::exists(sourceRoot() + QStringLiteral("/app/app.pro")), qPrintable(sourceRoot()));
    }

    void commentStripperKeepsLiteralsAndDropsComments()
    {
        QString source = QStringLiteral(
                    "a = \"http://kept.example\"; // http://dropped.example\n"
                    "/* https://dropped.example\n */ b = 'x'; c = \"// not a comment\";\n"
                    "d = \"esc\\\"aped // still string\"; // don't\n"
                    "e = `tmpl http://kept2.example`;\n");
        QString stripped = stripComments(source);
        QVERIFY(stripped.contains(QStringLiteral("http://kept.example")));
        QVERIFY(stripped.contains(QStringLiteral("http://kept2.example")));
        QVERIFY(stripped.contains(QStringLiteral("// not a comment")));
        QVERIFY(stripped.contains(QStringLiteral("// still string")));
        QVERIFY(!stripped.contains(QStringLiteral("dropped")));
        QVERIFY(!stripped.contains(QStringLiteral("don't")));
        QCOMPARE(stripped.count('\n'), source.count('\n'));
    }

    void scannerDetectsViolations()
    {
        // Guard against a scanner that silently matches nothing
        QTemporaryDir dir;
        QString path = dir.filePath(QStringLiteral("bad.cpp"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("// see https://example.org/doc\n"
                   "QUrl u(\"https://moonlight-stream.org/updates/qt.json\");\n"
                   "LiFindExternalAddressIP4(\"STUN.example.net\", 3478, &a);\n");
        file.close();

        QCOMPARE(findForbiddenReferences({path}).size(), 2);
        QStringList urls = findUnexpectedUrls({path}, true);
        QCOMPARE(urls.size(), 1);
        QVERIFY(urls.first().endsWith(QStringLiteral("https://moonlight-stream.org/updates/qt.json")));

        // Help links are only accepted in QML
        QString cppWithHelpLink = dir.filePath(QStringLiteral("help.cpp"));
        QFile helpFile(cppWithHelpLink);
        QVERIFY(helpFile.open(QIODevice::WriteOnly));
        helpFile.write(QByteArray("const char* u = \"") + kAllowedUrls[0] + "\";\n");
        helpFile.close();
        QCOMPARE(findUnexpectedUrls({cppWithHelpLink}, true).size(), 1);
    }

    void appSourcesHaveNoRemovedServices()
    {
        QStringList files = appSources();
        QVERIFY2(files.size() > 100, qPrintable(QString::number(files.size())));
        QStringList problems = findForbiddenReferences(files);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void appSourcesHaveOnlyAllowlistedUrls()
    {
        QStringList problems = findUnexpectedUrls(appSources(), true);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void commonCSourcesHaveNoRemovedServicesOrUrls()
    {
        QStringList files = commonCSources();
        QVERIFY2(files.size() > 30, qPrintable(QString::number(files.size())));
        for (const QString& file : files) {
            QVERIFY2(QFile::exists(file), qPrintable(file));
        }

        QStringList problems = findForbiddenReferences(files);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));

        problems = findUnexpectedUrls(files, false);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void commonCProjectOmitsStunAndConnectionTester()
    {
        QStringList listed;
        commonCSources(&listed);
        QVERIFY(listed.contains(QStringLiteral("src/Connection.c")));
        QVERIFY(listed.contains(QStringLiteral("src/PortFlags.c")));
        QVERIFY(!listed.contains(QStringLiteral("src/SimpleStun.c")));
        QVERIFY(!listed.contains(QStringLiteral("src/ConnectionTester.c")));

        const QString src = sourceRoot() + QStringLiteral("/moonlight-common-c/moonlight-common-c/src/");
        QVERIFY(!QFile::exists(src + QStringLiteral("SimpleStun.c")));
        QVERIFY(!QFile::exists(src + QStringLiteral("ConnectionTester.c")));

        QString header = readFile(src + QStringLiteral("Limelight.h"));
        QVERIFY(!header.isEmpty());
        QVERIFY(!stripComments(header).contains(QStringLiteral("LiFindExternalAddressIP4")));
        QVERIFY(!stripComments(header).contains(QStringLiteral("LiTestClientConnectivity")));
    }

    void appProjectOmitsRemovedComponents()
    {
        QString pro = readFile(sourceRoot() + QStringLiteral("/app/app.pro"));
        QVERIFY(!pro.isEmpty());
        for (const char* removed : {"autoupdatechecker", "compatfetcher", "mappingfetcher",
                                    "richpresencemanager", "discord"}) {
            QVERIFY2(!pro.contains(QString::fromLatin1(removed), Qt::CaseInsensitive), removed);
        }
    }
};

int runSourcePolicyTests(int argc, char** argv)
{
    TestSourcePolicy tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_sourcepolicy.moc"
