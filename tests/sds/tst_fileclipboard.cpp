#include "streaming/sds/fileclipboard.h"
#include "streaming/sds/clipboard_platform.h"
#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_DARWIN
#include <sys/xattr.h>
QStringList sdsTestPasteboardTypes(const QString& pasteboardName);
QStringList sdsTestPasteboardFiles(const QString& name);
bool sdsTestWriteFileUrls(const QString& name, const QStringList& paths);
bool sdsTestWritePng(const QString& name, const QByteArray& png);
QByteArray sdsTestReadPng(const QString& name);
#endif

using namespace Sds;

class TestFileClipboard : public QObject {
    Q_OBJECT
private slots:
    void codecRoundTrip();
    void codecRejects_data();
    void codecRejects();
    void sourceSnapshot();
    void nativeFileUrls();
    void tmpCanonicalParentAndLazyProvider();
};

void TestFileClipboard::codecRoundTrip()
{
    QList<ClipboardFile> files{{QStringLiteral("empty.txt"), {}}, {QString::fromUtf8("report-é.txt"), QByteArray("a\0b", 3)}};
    auto json = encodeFileClipboard(files);
    QVERIFY(!json.isEmpty());
    QList<ClipboardFile> decoded;
    QVERIFY(decodeFileClipboard(json, &decoded));
    QCOMPARE(decoded.size(), 2);
    QCOMPARE(decoded[0].name, files[0].name);
    QCOMPARE(decoded[0].data, QByteArray());
    QCOMPARE(decoded[1].name, files[1].name);
    QCOMPARE(decoded[1].data, files[1].data);
    files = {{QStringLiteral("limit.bin"), QByteArray(kFileClipboardBytesMax, 'x')}};
    QVERIFY(!encodeFileClipboard(files).isEmpty());
    files[0].data.append('x');
    QVERIFY(encodeFileClipboard(files).isEmpty());
    files.clear();
    for (int i = 0; i < kFileClipboardCountMax; ++i) files.append({QString::number(i), {}});
    QVERIFY(!encodeFileClipboard(files).isEmpty());
    files.append({QStringLiteral("extra"), {}});
    QVERIFY(encodeFileClipboard(files).isEmpty());
}

void TestFileClipboard::codecRejects_data()
{
    QTest::addColumn<QByteArray>("json");
    const QList<QPair<const char*, QByteArray>> cases{
        {"root-array", "[]"}, {"empty", "{\"files\":[]}"},
        {"unknown-root", "{\"files\":[{\"name\":\"a\",\"data\":\"\"}],\"path\":\"/tmp\"}"},
        {"unknown-entry", "{\"files\":[{\"name\":\"a\",\"data\":\"\",\"path\":\"/tmp\"}]}"},
        {"traversal", "{\"files\":[{\"name\":\"../a\",\"data\":\"\"}]}"},
        {"backslash", "{\"files\":[{\"name\":\"a\\\\b\",\"data\":\"\"}]}"},
        {"hidden", "{\"files\":[{\"name\":\".secret\",\"data\":\"\"}]}"},
        {"case-alias", "{\"files\":[{\"name\":\"a\",\"data\":\"\"},{\"name\":\"A\",\"data\":\"\"}]}"},
        {"duplicate-root", "{\"files\":[],\"files\":[{\"name\":\"a\",\"data\":\"\"}]}"},
        {"escaped-duplicate", "{\"files\":[{\"name\":\"a\",\"na\\u006de\":\"b\",\"data\":\"\"}]}"},
        {"invalid-base64", "{\"files\":[{\"name\":\"a\",\"data\":\"!!!!\"}]}"},
        {"unpadded", "{\"files\":[{\"name\":\"a\",\"data\":\"YQ\"}]}"},
        {"pad-bits", "{\"files\":[{\"name\":\"a\",\"data\":\"YR==\"}]}"},
        {"whitespace-base64", "{\"files\":[{\"name\":\"a\",\"data\":\"YQ==\\n\"}]}"},
        {"surrogate", "{\"files\":[{\"name\":\"\\ud800\",\"data\":\"\"}]}"},
        {"nul", "{\"files\":[{\"name\":\"a\\u0000b\",\"data\":\"\"}]}"},
        {"depth-object", "{\"files\":[{\"name\":\"a\",\"data\":\"\",\"x\":{\"y\":1}}]}"},
        {"depth-array", "{\"files\":[{\"name\":[\"a\"],\"data\":\"\"}]}"},
        {"overlong-key", QByteArray("{\"files\":[{\"") + QByteArray(65, 'k') + "\":\"\",\"name\":\"a\",\"data\":\"\"}]}" },
        {"too-many-keys", []() {
            QByteArray items;
            for (int i = 0; i < 16; ++i) {
                if (i > 0) items += ',';
                items += "{\"name\":\"f" + QByteArray::number(i) + "\",\"data\":\"\"}";
            }
            return QByteArray("{\"files\":[") + items + "],\"k1\":1,\"k2\":2}";
        }()},
        {"wire-limit", QByteArray(kFileClipboardWireMax + 1, ' ')},
    };
    for (const auto& item : cases) QTest::newRow(item.first) << item.second;
}

void TestFileClipboard::codecRejects()
{
    QFETCH(QByteArray, json);
    QList<ClipboardFile> files{{QStringLiteral("previous"), "private"}};
    QVERIFY(!decodeFileClipboard(json, &files));
    QVERIFY(files.isEmpty());
}

void TestFileClipboard::sourceSnapshot()
{
#ifdef Q_OS_UNIX
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    // /var is a system symlink on macOS. Use its canonical directory so the
    // positive fixture genuinely satisfies the all-components no-follow rule.
    const auto root = QFileInfo(temp.path()).canonicalFilePath();
    const auto path = root + QStringLiteral("/synthetic.txt");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write("fixture"), qint64(7));
    f.close();
    QString error;
    auto json = snapshotClipboardFiles({path}, &error);
    QVERIFY2(!json.isEmpty(), qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(!json.contains(root.toUtf8()));
    QList<ClipboardFile> decoded;
    QVERIFY(decodeFileClipboard(json, &decoded));
    QCOMPARE(decoded[0].data, QByteArray("fixture"));
    const auto link = root + QStringLiteral("/alias.txt");
    QVERIFY(::symlink(QFile::encodeName(path).constData(), QFile::encodeName(link).constData()) == 0);
    QVERIFY(snapshotClipboardFiles({link}, &error).isEmpty());
    QVERIFY(snapshotClipboardFiles({path, link}, &error).isEmpty());
    QVERIFY(snapshotClipboardFiles({root}, &error).isEmpty());
    const auto parentLink = root + QStringLiteral("/parent");
    QVERIFY(::symlink(QFile::encodeName(root).constData(), QFile::encodeName(parentLink).constData()) == 0);
    QVERIFY(snapshotClipboardFiles({parentLink + QStringLiteral("/synthetic.txt")}, &error).isEmpty());
    const auto fifo = root + QStringLiteral("/pipe");
    QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
    QVERIFY(snapshotClipboardFiles({fifo}, &error).isEmpty());
    QVERIFY(snapshotClipboardFiles({path, path}, &error).isEmpty());

    // NFD filename (as returned by macOS APFS) opens and normalizes to NFC on the wire
    const QString nfdName = QString::fromUtf8("e\xCC\x81te\xCC\x81.txt");
    const QString nfdPath = root + QLatin1Char('/') + nfdName;
    QFile nfdFile(nfdPath);
    QVERIFY(nfdFile.open(QIODevice::WriteOnly));
    QCOMPARE(nfdFile.write("nfd-data"), qint64(8));
    nfdFile.close();
    auto nfdJson = snapshotClipboardFiles({nfdPath}, &error);
    QVERIFY2(!nfdJson.isEmpty(), qPrintable(error));
    QList<ClipboardFile> nfdDecoded;
    QVERIFY(decodeFileClipboard(nfdJson, &nfdDecoded));
    QCOMPARE(nfdDecoded.size(), 1);
    QCOMPARE(nfdDecoded[0].name, QStringLiteral("été.txt"));
    QCOMPARE(nfdDecoded[0].data, QByteArray("nfd-data"));
#else
    QSKIP("POSIX source snapshot test");
#endif
}

void TestFileClipboard::nativeFileUrls()
{
#ifdef Q_OS_DARWIN
    const auto name = QStringLiteral("SDS-file-clipboard-test-") + QUuid::createUuid().toString();
    auto backend = createPlatformClipboard(name);
    const auto bundle = encodeFileClipboard({{QStringLiteral("synthetic.txt"), "bytes"}, {QStringLiteral("empty.txt"), {}}});
    QString error;
    const auto generation = backend->writeFiles(bundle, backend->changeCount(), &error);
    QVERIFY2(generation >= 0, qPrintable(error));
    QVERIFY(backend->hasFileSelection());
    auto paths = sdsTestPasteboardFiles(name);
    QCOMPARE(paths.size(), 2);
    const QString expectedRoot = QFileInfo(
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/file-clipboard"))
        .canonicalFilePath();
    QVERIFY(!expectedRoot.isEmpty());
    QVERIFY2(QFileInfo(paths[0]).canonicalFilePath().startsWith(expectedRoot + QLatin1Char('/')),
             qPrintable(paths[0]));
    QFile f(paths[0]);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("bytes"));
    struct stat st{};
    QVERIFY(::stat(QFile::encodeName(paths[0]).constData(), &st) == 0);
    QCOMPARE(st.st_mode & 0777, mode_t(0600));
    QVERIFY(::stat(QFile::encodeName(QFileInfo(paths[0]).absolutePath()).constData(), &st) == 0);
    QCOMPARE(st.st_mode & 0777, mode_t(0700));
    QVERIFY(::getxattr(QFile::encodeName(paths[0]).constData(), "com.apple.quarantine", nullptr, 0, 0, 0) > 0);
    QVERIFY(backend->readFiles(&error).isEmpty()); // never echo our own staging URLs
    QVERIFY(backend->read(1024, 1024).item.isEmpty()); // never send file paths as text
    backend->retireFiles();
    QVERIFY(sdsTestPasteboardFiles(name).isEmpty());
    QVERIFY(QFileInfo::exists(paths[0])); // native readers get a bounded grace
    QTemporaryDir source;
    const auto sourceFile = QFileInfo(source.path()).canonicalFilePath() + QStringLiteral("/local.txt");
    QFile local(sourceFile);
    QVERIFY(local.open(QIODevice::WriteOnly));
    QCOMPARE(local.write("local"), qint64(5));
    local.close();
    QVERIFY(sdsTestWriteFileUrls(name, {sourceFile}));
    QVERIFY(!backend->readFiles(&error).isEmpty());
    const auto beforeStale = backend->changeCount();
    QVERIFY(backend->writeFiles(bundle, beforeStale - 1, &error) < 0);
    QCOMPARE(backend->changeCount(), beforeStale);
    QCOMPARE(sdsTestPasteboardFiles(name), QStringList{sourceFile});
    backend->retireFiles();
    QCOMPARE(sdsTestPasteboardFiles(name), QStringList{sourceFile}); // preserve user's Copy

    // Stale leftover stage directories inside the private cache root are swept on construction
    const QString staleStage = expectedRoot + QStringLiteral("/.sds-file-clipboard-stale123");
    QVERIFY(QDir().mkpath(staleStage));
    {
        QFile staleFile(staleStage + QStringLiteral("/old.txt"));
        QVERIFY(staleFile.open(QIODevice::WriteOnly));
        staleFile.write("leftover");
    }
    auto sweeper = createPlatformClipboard(name + QStringLiteral("-sweep"));
    QVERIFY(!QFileInfo::exists(staleStage));
#else
    QSKIP("macOS named pasteboard only");
#endif
}

void TestFileClipboard::tmpCanonicalParentAndLazyProvider()
{
#ifdef Q_OS_UNIX
    // /tmp is a root-owned symlink to /private/tmp on macOS: regular files under /tmp must work,
    // while symlink leaves inside /tmp remain rejected.
    QTemporaryDir tmpRoot(QStringLiteral("/tmp/sds-fileclip-tmp-XXXXXX"));
    QVERIFY(tmpRoot.isValid());
    const QString tmpFile = tmpRoot.path() + QStringLiteral("/via-tmp.txt");
    QFile tf(tmpFile);
    QVERIFY(tf.open(QIODevice::WriteOnly));
    QCOMPARE(tf.write("tmp-bytes"), qint64(9));
    tf.close();

    QString err;
    const QByteArray bundle = snapshotClipboardFiles({tmpFile}, &err);
    QVERIFY2(!bundle.isEmpty(), qPrintable(err));
    QList<ClipboardEntryMeta> metas;
    QVERIFY2(inspectClipboardFiles({tmpFile}, kClientFileMax, &metas, &err), qPrintable(err));
    QCOMPARE(metas.size(), 1);
    QCOMPARE(metas[0].name, QStringLiteral("via-tmp.txt"));
    QCOMPARE(metas[0].size, qint64(9));

    const QString tmpLeafLink = tmpRoot.path() + QStringLiteral("/leaf-link.txt");
    QCOMPARE(::symlink(QFile::encodeName(tmpFile).constData(), QFile::encodeName(tmpLeafLink).constData()), 0);
    QVERIFY(snapshotClipboardFiles({tmpLeafLink}, &err).isEmpty());
    QVERIFY(!inspectClipboardFiles({tmpLeafLink}, kClientFileMax, &metas, &err));
#endif

#ifdef Q_OS_DARWIN
    // Test MacClipboard NSPasteboardItemDataProvider lazy promise & 4.5s timeout concrete fallback.
    const QString pbName = QStringLiteral("SDS-lazy-pb-") + QUuid::createUuid().toString();
    auto backend = createPlatformClipboard(pbName);

    QString stageErr;
    const QString stageDir = backend->createLazyStageDir(&stageErr);
    QVERIFY2(!stageDir.isEmpty(), qPrintable(stageErr));
    const QString stagedFile = stageDir + QStringLiteral("/promised.txt");
    QFile sf(stagedFile);
    QVERIFY(sf.open(QIODevice::WriteOnly));
    sf.write("lazy-content");
    sf.close();

    LazyClipboardOffer offer;
    offer.offerId = QStringLiteral("offer_lazy_1");
    offer.kind = QStringLiteral("files");
    offer.fromHost = true;
    offer.entries.append({QStringLiteral("e0"), QStringLiteral("promised.txt"), 12, QString()});

    auto transfer = std::make_shared<LazyTransferResult>();
    int demandCalls = 0;
    const qint64 cc = backend->writeLazyOffer(offer, transfer, [&](const QString& oid) {
        ++demandCalls;
        QCOMPARE(oid, QStringLiteral("offer_lazy_1"));
        {
            std::lock_guard<std::mutex> lock(transfer->mutex);
            transfer->ok = true;
            transfer->stagedPaths = QStringList{stagedFile};
            transfer->finished = true;
        }
        transfer->cv.notify_all();
    }, &err);
    QVERIFY2(cc >= 0, qPrintable(err));
    QCOMPARE(demandCalls, 0);
    QVERIFY(!sdsTestPasteboardTypes(pbName).isEmpty());
    QCOMPARE(demandCalls, 0); // Reading types alone never invokes provideDataForType

    const QStringList resolved = sdsTestPasteboardFiles(pbName);
    QCOMPARE(demandCalls, 1);
    QCOMPARE(resolved, QStringList{stagedFile});
    QCOMPARE(backend->changeCount(), cc); // Promise fulfillment must not change changeCount

    qint64 newCc = -1;
    bool needRepaste = false;
    QVERIFY(backend->completeLazyOffer(offer.offerId, true, {stagedFile}, {}, {}, &newCc, &needRepaste));
    QVERIFY(!needRepaste); // Provider already fulfilled immediately without repaste prompt

    // Test PNG lazy offer (`kind = "image"`) and `readLazyImage` staging above threshold.
    const QByteArray smallPng = SdsTest::makePng(8, 8);
    QVERIFY(sdsTestWritePng(pbName, smallPng));
    QString tempImgPath;
    qint64 imgSize = 0;
    QVERIFY(!backend->readLazyImage(smallPng.size() + 1, kClientFileMax, &tempImgPath, &imgSize, &err));
    QVERIFY2(backend->readLazyImage(10, kClientFileMax, &tempImgPath, &imgSize, &err), qPrintable(err));
    QCOMPARE(imgSize, qint64(smallPng.size()));
    QFile imgFile(tempImgPath);
    QVERIFY(imgFile.open(QIODevice::ReadOnly));
    QCOMPARE(imgFile.readAll(), smallPng);
    struct stat imgSt{};
    QVERIFY(::stat(QFile::encodeName(tempImgPath).constData(), &imgSt) == 0);
    QCOMPARE(imgSt.st_mode & 0777, mode_t(0600));
    QVERIFY(::getxattr(QFile::encodeName(tempImgPath).constData(), "com.apple.quarantine", nullptr, 0, 0, 0) > 0);

    LazyClipboardOffer pngOffer;
    pngOffer.offerId = QStringLiteral("offer_png_1");
    pngOffer.kind = QStringLiteral("image");
    pngOffer.mime = QStringLiteral("image/png");
    pngOffer.fromHost = true;
    pngOffer.entries.append({QStringLiteral("img0"), QStringLiteral("clipboard.png"), smallPng.size(), QString()});
    auto pngTransfer = std::make_shared<LazyTransferResult>();
    int pngDemand = 0;
    QVERIFY(backend->writeLazyOffer(pngOffer, pngTransfer, [&](const QString& oid) {
        ++pngDemand;
        QCOMPARE(oid, QStringLiteral("offer_png_1"));
        {
            std::lock_guard<std::mutex> lock(pngTransfer->mutex);
            pngTransfer->ok = true;
            pngTransfer->png = smallPng;
            pngTransfer->finished = true;
        }
        pngTransfer->cv.notify_all();
    }, &err) >= 0);
    QCOMPARE(pngDemand, 0);
    QCOMPARE(sdsTestReadPng(pbName), smallPng);
    QCOMPARE(pngDemand, 1);

    // Timeout fallback case (>4.5s wait): provider returns cleanly, then completeLazyOffer
    // publishes concrete staged NSURLs and sets needRepasteToast = true.
    const QString pbTimeoutName = QStringLiteral("SDS-lazy-timeout-") + QUuid::createUuid().toString();
    auto backend2 = createPlatformClipboard(pbTimeoutName);
    const QString stageDir2 = backend2->createLazyStageDir(&stageErr);
    QVERIFY(!stageDir2.isEmpty());
    const QString stagedFile2 = stageDir2 + QStringLiteral("/late.txt");
    QFile sf2(stagedFile2);
    QVERIFY(sf2.open(QIODevice::WriteOnly));
    sf2.write("late-content");
    sf2.close();

    LazyClipboardOffer offer2 = offer;
    offer2.offerId = QStringLiteral("offer_lazy_2");
    offer2.entries = {{QStringLiteral("e0"), QStringLiteral("late.txt"), 12, QString()}};
    auto transfer2 = std::make_shared<LazyTransferResult>();
    int lateDemand = 0;
    QVERIFY(backend2->writeLazyOffer(offer2, transfer2, [&](const QString&) {
        ++lateDemand;
        // Leave transfer2 unfinished so provider times out cleanly at 4.5s without deadlock
    }, &err) >= 0);
    QVERIFY(sdsTestPasteboardFiles(pbTimeoutName).isEmpty());
    QCOMPARE(lateDemand, 1);
    {
        std::lock_guard<std::mutex> lock(transfer2->mutex);
        transfer2->ok = true;
        transfer2->stagedPaths = QStringList{stagedFile2};
        transfer2->finished = true;
    }
    transfer2->cv.notify_all();
    qint64 fallbackCc = -1;
    bool fallbackRepaste = false;
    QVERIFY(backend2->completeLazyOffer(offer2.offerId, true, {stagedFile2}, {}, {}, &fallbackCc, &fallbackRepaste));
    QVERIFY(fallbackRepaste);
    QVERIFY(fallbackCc >= 0);
    QCOMPARE(sdsTestPasteboardFiles(pbTimeoutName), QStringList{stagedFile2});
    backend2->retireFiles();
    QVERIFY(sdsTestPasteboardFiles(pbTimeoutName).isEmpty());
#endif
}

int runFileClipboardTests(int argc, char** argv)
{
    TestFileClipboard test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_fileclipboard.moc"
