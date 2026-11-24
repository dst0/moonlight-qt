#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <unistd.h>

#include "backend/boxartmanager.h"
#include "backend/nvcomputer.h"
#include "path.h"
#include "tests.h"

namespace {

bool writeBytes(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

} // namespace

class TestBoxArtAndComputer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("SecureDesktopStreamTests"));
        QCoreApplication::setApplicationName(QStringLiteral("boxart-unit-") +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
        Path::initialize(false);
        QDir(Path::getBoxArtCacheDir()).removeRecursively();
        QVERIFY(QDir().mkpath(Path::getBoxArtCacheDir()));
    }

    void cleanupTestCase()
    {
        QDir(Path::getBoxArtCacheDir()).removeRecursively();
    }

    void uuidValidation()
    {
        QVERIFY(NvComputer::isValidUuid(QStringLiteral("12345678-9abc-def0-1234-56789abcdef0")));
        QVERIFY(NvComputer::isValidUuid(QStringLiteral("Host_01-ABC")));
        QVERIFY(NvComputer::isValidUuid(QString(64, QLatin1Char('a'))));

        QVERIFY(!NvComputer::isValidUuid(QString()));
        QVERIFY(!NvComputer::isValidUuid(QString(65, QLatin1Char('a'))));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral(".")));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral("..")));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral("../escape")));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral("a/b")));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral("a\\b")));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral("host id")));
        QVERIFY(!NvComputer::isValidUuid(QStringLiteral("id\n")));
    }

    void nvComputerParsesFileClipboardAndFiltersAddresses()
    {
        NvHTTP http(NvAddress(QStringLiteral("192.168.1.50"), 47989), 47984, QSslCertificate(), true);
        const QString serverInfo = QStringLiteral(
            "<root status_code=\"200\">"
            "<hostname>TestHost</hostname>"
            "<uniqueid>12345678-9abc-def0-1234-56789abcdef0</uniqueid>"
            "<mac>aa:bb:cc:dd:ee:ff</mac>"
            "<LocalIP>8.8.8.8</LocalIP>"
            "<ExternalIP>1.2.3.4</ExternalIP>"
            "<HttpsPort>47984</HttpsPort>"
            "<PairStatus>1</PairStatus>"
            "<state>SUNSHINE_SERVER_FREE</state>"
            "<sdsVersion>1</sdsVersion>"
            "<sdsFeatures>clipboard, files, file_clipboard, FILE_CLIPBOARD, hw_ycocgr_444, unknown_feat</sdsFeatures>"
            "<sdsPacked444MaxSize>8192x4352</sdsPacked444MaxSize>"
            "</root>");

        NvComputer comp(http, serverInfo);
        QCOMPARE(comp.uuid, QStringLiteral("12345678-9abc-def0-1234-56789abcdef0"));
        QVERIFY(comp.localAddress.isNull());
        QVERIFY(comp.remoteAddress.isNull());
        QCOMPARE(comp.sdsVersion, 1);
        QCOMPARE(comp.sdsFeatures, (QStringList{
            QStringLiteral("clipboard"),
            QStringLiteral("files"),
            QStringLiteral("file_clipboard"),
            QStringLiteral("hw_ycocgr_444"),
        }));
        QCOMPARE(comp.maxPackedWidth, 8192);
        QCOMPARE(comp.maxPackedHeight, 4352);

        // Invalid sdsPacked444MaxSize falls back to 0x0
        const QString malformedPackedXml = QStringLiteral(
            "<root status_code=\"200\">"
            "<hostname>TestHost</hostname>"
            "<uniqueid>12345678-9abc-def0-1234-56789abcdef0</uniqueid>"
            "<sdsVersion>1</sdsVersion>"
            "<sdsFeatures>hw_ycocgr_444</sdsFeatures>"
            "<sdsPacked444MaxSize>badxsize</sdsPacked444MaxSize>"
            "</root>");
        NvComputer malformedComp(http, malformedPackedXml);
        QCOMPARE(malformedComp.maxPackedWidth, 0);
        QCOMPARE(malformedComp.maxPackedHeight, 0);
        QVERIFY(malformedComp.sdsFeatures.contains(QStringLiteral("hw_ycocgr_444")));

        // Invalid uniqueid in XML is rejected
        const QString badXml = QStringLiteral(
            "<root status_code=\"200\">"
            "<hostname>Evil</hostname>"
            "<uniqueid>../evil</uniqueid>"
            "<sdsVersion>1</sdsVersion>"
            "<sdsFeatures>file_clipboard</sdsFeatures>"
            "</root>");
        NvComputer badComp(http, badXml);
        QVERIFY(badComp.uuid.isEmpty());
        QVERIFY(!comp.update(badComp));

        // Persisted QSettings filtering: invalid UUID cleared, public addresses dropped
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(dir.filePath(QStringLiteral("hosts.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("hostname"), QStringLiteral("Saved"));
        settings.setValue(QStringLiteral("uuid"), QStringLiteral("12345678-9abc-def0-1234-56789abcdef0"));
        settings.setValue(QStringLiteral("localaddress"), QStringLiteral("192.168.1.10"));
        settings.setValue(QStringLiteral("ipv6address"), QStringLiteral("2001:4860:4860::8888"));
        settings.setValue(QStringLiteral("manualaddress"), QStringLiteral("8.8.4.4"));
        NvComputer persisted(settings);
        QCOMPARE(persisted.uuid, QStringLiteral("12345678-9abc-def0-1234-56789abcdef0"));
        QCOMPARE(persisted.localAddress.address(), QStringLiteral("192.168.1.10"));
        QVERIFY(persisted.ipv6Address.isNull());
        QVERIFY(persisted.manualAddress.isNull());

        settings.setValue(QStringLiteral("uuid"), QStringLiteral("../traversal"));
        settings.setValue(QStringLiteral("ipv6address"), QStringLiteral("fd00::1234"));
        settings.setValue(QStringLiteral("manualaddress"), QStringLiteral("10.0.0.5"));
        NvComputer persistedBadUuid(settings);
        QVERIFY(persistedBadUuid.uuid.isEmpty());
        QCOMPARE(persistedBadUuid.ipv6Address.address(), QStringLiteral("fd00::1234"));
        QCOMPARE(persistedBadUuid.manualAddress.address(), QStringLiteral("10.0.0.5"));
    }

    void boxArtUsesSha256SubdirAndRejectsTraversal()
    {
        BoxArtManager bam;
        const QString cacheRoot = QFileInfo(Path::getBoxArtCacheDir()).canonicalFilePath();
        QVERIFY(!cacheRoot.isEmpty());

        const QString validUuid = QStringLiteral("12345678-9abc-def0-1234-56789abcdef0");
        const QString expectedHash = QString::fromLatin1(
            QCryptographicHash::hash(validUuid.toUtf8(), QCryptographicHash::Sha256).toHex());
        const QString expectedDir = QDir(cacheRoot).filePath(expectedHash);
        const QString cachedPng = QDir(expectedDir).filePath(QStringLiteral("42.png"));
        QVERIFY(writeBytes(cachedPng, QByteArray("PNGDATA")));

        NvComputer validComp;
        validComp.uuid = validUuid;
        NvApp app;
        app.id = 42;
        app.name = QStringLiteral("Test");

        const QUrl loaded = bam.loadBoxArt(&validComp, app);
        QCOMPARE(loaded, QUrl::fromLocalFile(cachedPng));
        QCOMPARE(QFileInfo(loaded.toLocalFile()).dir().canonicalPath(), expectedDir);
        QCOMPARE(QFileInfo(expectedDir).dir().canonicalPath(), cacheRoot);

        BoxArtManager::deleteBoxArt(&validComp);
        QVERIFY(!QFileInfo::exists(expectedDir));

        // Traversal UUID must never read or delete outside cacheRoot
        QTemporaryDir outsideDir;
        QVERIFY(outsideDir.isValid());
        const QString outsideEscape = QDir(cacheRoot).filePath(QStringLiteral("../boxart-escape-test"));
        const QString outsideFile = QDir(outsideEscape).filePath(QStringLiteral("42.png"));
        QVERIFY(writeBytes(outsideFile, QByteArray("OUTSIDE")));

        for (const QString& badUuid : {QStringLiteral("../boxart-escape-test"),
                                       QStringLiteral("sub/dir"),
                                       QStringLiteral(".."),
                                       QString()}) {
            NvComputer badComp;
            badComp.uuid = badUuid;
            QCOMPARE(bam.loadBoxArt(&badComp, app), QUrl(QStringLiteral("qrc:/res/no_app_image.png")));
            BoxArtManager::deleteBoxArt(&badComp);
            QVERIFY(QFileInfo::exists(outsideFile));
        }
        QDir(outsideEscape).removeRecursively();
    }

    void boxArtRejectsSymlinkedDirAndSymlinkedFile()
    {
        BoxArtManager bam;
        const QString cacheRoot = QFileInfo(Path::getBoxArtCacheDir()).canonicalFilePath();
        QVERIFY(!cacheRoot.isEmpty());

        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString targetDir = QFileInfo(outside.path()).canonicalFilePath() + QStringLiteral("/target");
        const QString targetFile = targetDir + QStringLiteral("/77.png");
        QVERIFY(writeBytes(targetFile, QByteArray("SECRET")));

        const QString uuid = QStringLiteral("abcdef01-2345-6789-abcd-ef0123456789");
        const QString hashDir = QDir(cacheRoot).filePath(QString::fromLatin1(
            QCryptographicHash::hash(uuid.toUtf8(), QCryptographicHash::Sha256).toHex()));
        QDir(hashDir).removeRecursively();
        QCOMPARE(::symlink(QFile::encodeName(targetDir).constData(),
                           QFile::encodeName(hashDir).constData()), 0);

        NvComputer comp;
        comp.uuid = uuid;
        NvApp app;
        app.id = 77;

        QCOMPARE(bam.loadBoxArt(&comp, app), QUrl(QStringLiteral("qrc:/res/no_app_image.png")));
        BoxArtManager::deleteBoxArt(&comp);
        QVERIFY(QFileInfo::exists(targetFile));
        QFile::remove(hashDir);

        // Real dir with a symlinked file inside it must also be rejected
        QVERIFY(QDir().mkpath(hashDir));
        const QString linkFile = QDir(hashDir).filePath(QStringLiteral("77.png"));
        QCOMPARE(::symlink(QFile::encodeName(targetFile).constData(),
                           QFile::encodeName(linkFile).constData()), 0);
        QCOMPARE(bam.loadBoxArt(&comp, app), QUrl(QStringLiteral("qrc:/res/no_app_image.png")));
        BoxArtManager::deleteBoxArt(&comp);
        QVERIFY(QFileInfo::exists(targetFile));
    }
};

int runBoxArtTests(int argc, char** argv)
{
    TestBoxArtAndComputer tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_boxart.moc"
