// Integration tests: SdsClient against the Python mock host (mock_sds_host.py) over mTLS with
// a pinned server certificate.

#include "backend/sdsclient.h"
#include "streaming/sds/filesanitize.h"
#include "streaming/sds/fileclipboard.h"
#include "streaming/video/adaptiverefinement_codec.h"
#include "testutil.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef Q_OS_DARWIN
#include <sys/xattr.h>
#endif

using namespace Sds;
using SdsTest::EventCollector;

namespace {

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool hasQuarantine(const QString& path)
{
#ifdef Q_OS_DARWIN
    char buf[256];
    ssize_t n = getxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", buf, sizeof(buf), 0, 0);
    return n > 5 && QByteArray(buf, static_cast<int>(n)).startsWith("0081;");
#else
    Q_UNUSED(path);
    return true;
#endif
}

bool isExecutable(const QString& path)
{
    struct stat st;
    return ::stat(QFile::encodeName(path).constData(), &st) != 0 || (st.st_mode & 0111) != 0;
}

QJsonObject obj(std::initializer_list<QPair<QString, QJsonValue>> kv)
{
    QJsonObject o;
    for (const auto& p : kv) {
        o.insert(p.first, p.second);
    }
    return o;
}

QJsonArray strings(std::initializer_list<const char*> list)
{
    QJsonArray a;
    for (const char* s : list) {
        a.append(QString::fromLatin1(s));
    }
    return a;
}

EventCollector::Predicate transfer(bool upload, bool ok)
{
    return [upload, ok](const ClientEvent& e) {
        return e.type == ClientEvent::TransferFinished && e.upload == upload && e.ok == ok;
    };
}

EventCollector::Predicate clipboardText(const QByteArray& text)
{
    return [text](const ClientEvent& e) {
        return e.type == ClientEvent::ClipboardReceived && e.clipboard.text == text;
    };
}

} // namespace

class TestSdsClient : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void capabilitiesAndReady();
    void hostClipboardAtStartWins();
    void longPollDeliversEvents();
    void clipboardGone410();
    void clipboardPutSendsBothRepresentations();
    void clipboardRejectedClientSide();
    void hostClipboardValidated();
    void clipboardErrorCodes();
    void uploadResumesAfterConflict();
    void uploadNamesAndCollisions();
    void uploadRefusalsAndCapabilitiesEvent();
    void uploadVerificationFailure();
    void uploadEmptyFile();
    void outboxDownloadsAndSanitizes();
    void outboxCollisionAndDedupe();
    void outboxIntegrityFailures();
    void outboxManualDownload();
    void featureDisabledStopsQuietly();
    void noActiveStreamThenActive();
    void rateLimitedCapabilities();
    void streamEndedStopsClient();
    void pinningRejectsWrongServerCert();
    void hostRejectsUnknownClient();
    void stopIsPromptDuringLongPoll();
    void outboxRangeRetry();
    void outboxRequiresChecksumAndSize();
    void outboxIgnoresBadIdsAndHandlesEmptyFile();
    void unsupportedVersion();
    void eventNumberingReset();
    void hostNamesAreRejectedBeforeAnyRequest();
    void nativeFileClipboardProtocol();
    void nativeFileClipboardGatesAndStaleWrite();
    void nativeFilePendingCopyCanceled();
    void eventsPollFloorStopsSpinLoop();
    void capabilitiesEventsCoalesced();
    void fileClipboardIgnoresNonAdvancingSeqOn200();
    void uploadRejectsSymlinkAndFifo();
    void lazyClipboardClientOfferAndChunkedPull();
    void lazyClipboardHostOfferAndDemandFetch();
    void unifiedDropsMultiFileAndFallback();
    void tileRefinementLongPollOverPinnedMtls();

private:
    bool startMock(const SdsTest::Identity& server, const SdsTest::Identity& trustedClient);
    void startClient(bool autoDownload = true, int longPollTimeoutSec = 2, bool nativeFiles = false, bool xdndDrop = false);
    bool waitReady();
    QList<QJsonObject> requests(const QString& method, const QString& pathPrefix);
    QJsonArray clientPuts();

    QTemporaryDir m_CertDir;
    SdsTest::Identity m_ServerA;
    SdsTest::Identity m_ServerB;
    SdsTest::Identity m_Client;
    SdsTest::Identity m_OtherClient;

    QTemporaryDir* m_Tmp = nullptr;
    QString m_Downloads;
    SdsTest::MockHost* m_Host = nullptr;
    EventCollector m_Events;
    std::unique_ptr<SdsClient> m_Sds;
};

void TestSdsClient::initTestCase()
{
    QVERIFY(m_CertDir.isValid());
    QVERIFY(SdsTest::generateIdentity(m_CertDir.path(), QStringLiteral("server-a"), QStringLiteral("Sunshine Gamestream Host"), &m_ServerA));
    QVERIFY(SdsTest::generateIdentity(m_CertDir.path(), QStringLiteral("server-b"), QStringLiteral("Sunshine Gamestream Host"), &m_ServerB));
    QVERIFY(SdsTest::generateIdentity(m_CertDir.path(), QStringLiteral("client"), QStringLiteral("NVIDIA GameStream Client"), &m_Client));
    QVERIFY(SdsTest::generateIdentity(m_CertDir.path(), QStringLiteral("other-client"), QStringLiteral("NVIDIA GameStream Client"), &m_OtherClient));
    qInfo() << "TLS backend:" << QSslSocket::activeBackend() << QSslSocket::sslLibraryVersionString();
}

void TestSdsClient::init()
{
    m_Events.clear();
    m_Tmp = new QTemporaryDir();
    QVERIFY(m_Tmp->isValid());
    m_Downloads = m_Tmp->filePath(QStringLiteral("Downloads/Moonlight"));
    QVERIFY(startMock(m_ServerA, m_Client));
}

void TestSdsClient::cleanup()
{
    if (m_Sds) {
        m_Sds->stop();
        m_Sds.reset();
    }
    delete m_Host;
    m_Host = nullptr;
    delete m_Tmp;
    m_Tmp = nullptr;
}

bool TestSdsClient::startMock(const SdsTest::Identity& server, const SdsTest::Identity& trustedClient)
{
    delete m_Host;
    m_Host = new SdsTest::MockHost();
    QString error;
    if (!m_Host->start(server, trustedClient, &error)) {
        qWarning() << error;
        return false;
    }
    return true;
}

void TestSdsClient::startClient(bool autoDownload, int longPollTimeoutSec, bool nativeFiles, bool xdndDrop)
{
    SdsClient::Config cfg;
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.httpsPort = m_Host->port();
    cfg.serverCert = m_ServerA.cert;
    cfg.clientCert = m_Client.cert;
    cfg.clientKey = m_Client.key;
    cfg.downloadDir = m_Downloads;
    cfg.autoDownload = autoDownload;
    cfg.fileClipboardEnabled = nativeFiles;
    cfg.xdndDropEnabled = xdndDrop;
    cfg.longPollTimeoutSec = longPollTimeoutSec;
    cfg.retryBaseMs = 100;
    cfg.retryMaxMs = 400;
    cfg.retryAfterMaxSec = 1;

    m_Sds = std::make_unique<SdsClient>(cfg, &m_Events);
    m_Sds->start();
}

void TestSdsClient::nativeFileClipboardProtocol()
{
    const auto bundle = encodeFileClipboard({{QStringLiteral("synthetic.txt"), "bytes"}});
    const auto command = QJsonObject{{QStringLiteral("cmd"), QStringLiteral("file_clipboard")},
                                    {QStringLiteral("bundle"), QJsonDocument::fromJson(bundle).object()}};
    m_Host->cmd(command); // old clipboard must not replay on first GET
    startClient(false, 1, true);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Ready)));
    m_Sds->exchangeFileClipboard({}, 100);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::FileClipboardObserved)));
    for (const auto& ev : m_Events.events()) QVERIFY(ev.fileBundle.isEmpty());
    m_Events.clear();
    m_Host->cmd(command);
    m_Sds->exchangeFileClipboard({}, 101);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return !e.fileBundle.isEmpty(); }));
    const auto events = m_Events.events();
    bool found = false;
    for (const auto& ev : events) {
        if (ev.type == ClientEvent::FileClipboardObserved && !ev.fileBundle.isEmpty()) {
            QList<ClipboardFile> decoded;
            QVERIFY(decodeFileClipboard(ev.fileBundle, &decoded));
            QCOMPARE(decoded[0].data, QByteArray("bytes"));
            QCOMPARE(ev.localGeneration, qint64(101));
            found = true;
        }
    }
    QVERIFY(found);
    m_Events.clear();
    m_Sds->exchangeFileClipboard(bundle, 102);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::FileClipboardObserved)));
    QCOMPARE(m_Host->state()[QStringLiteral("file_clip_puts")].toArray().size(), 1);
    const auto puts = requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/file-clipboard"));
    QCOMPARE(puts.size(), 1);
    QVERIFY(!puts[0][QStringLiteral("file_clip_epoch")].toString().isEmpty());
    QCOMPARE(puts[0][QStringLiteral("file_clip_seq")].toString(), QStringLiteral("2"));
    // A new epoch invalidates the session baseline and must not publish the returned data.
    auto changed = command;
    changed.insert(QStringLiteral("new_epoch"), true);
    m_Host->cmd(changed);
    m_Events.clear();
    m_Sds->exchangeFileClipboard({}, 103);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return e.localGeneration == -2; }));
    for (const auto& ev : m_Events.events()) QVERIFY(ev.fileBundle.isEmpty());
}

void TestSdsClient::nativeFileClipboardGatesAndStaleWrite()
{
    const auto bundle = encodeFileClipboard({{QStringLiteral("a.txt"), "bytes"}});
    startClient(false, 1, false);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Ready)));
    m_Sds->exchangeFileClipboard(bundle, 1);
    QTest::qWait(150);
    QCOMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/file-clipboard")).size(), 0);
    m_Sds->stop(); m_Sds.reset(); m_Events.clear();
    startClient(false, 1, true);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Ready)));
    m_Sds->exchangeFileClipboard({}, 1);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::FileClipboardObserved)));
    m_Host->fault({{QStringLiteral("kind"), QStringLiteral("status")},
                   {QStringLiteral("method"), QStringLiteral("PUT")},
                   {QStringLiteral("path_prefix"), QStringLiteral("/sds/v1/file-clipboard")},
                   {QStringLiteral("status"), 409}});
    m_Events.clear();
    m_Sds->exchangeFileClipboard(bundle, 2);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return e.localGeneration == -2; }));
    m_Sds->exchangeFileClipboard({}, 3);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return e.localGeneration == 3; }));
    QTest::qWait(200);
    QCOMPARE(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/file-clipboard")).size(), 1);
    QVERIFY(m_Host->state()[QStringLiteral("file_clip_puts")].toArray().isEmpty());
}

void TestSdsClient::nativeFilePendingCopyCanceled()
{
    startClient(false, 1, true);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Ready)));
    m_Sds->exchangeFileClipboard({}, 1);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::FileClipboardObserved)));
    m_Host->config({{QStringLiteral("file_clip_block"), true}});
    m_Events.clear();
    m_Sds->exchangeFileClipboard({}, 1);
    QTRY_COMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/file-clipboard")).size(), 2);
    const auto bundle = encodeFileClipboard({{QStringLiteral("canceled.txt"), "private"}});
    m_Sds->exchangeFileClipboard(bundle, 2);
    m_Sds->exchangeFileClipboard({}, 3); // user copied newer text while GET was blocked
    m_Host->config({{QStringLiteral("file_clip_block"), false}});
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::FileClipboardObserved)));
    m_Events.clear();
    m_Sds->exchangeFileClipboard({}, 3);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return e.localGeneration == 3; }));
    QCOMPARE(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/file-clipboard")).size(), 0);
    QVERIFY(m_Host->state()[QStringLiteral("file_clip_puts")].toArray().isEmpty());
}

void TestSdsClient::eventsPollFloorStopsSpinLoop()
{
    startClient(false, 2, false);
    QVERIFY(waitReady());
    QTest::qWait(250); // let the parked long poll sit

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("clear_log") } }));
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("spin_event") },
                        { QStringLiteral("count"), 25 } }));
    // Wake the parked long poll with one dummy event so spin_event triggers immediately on subsequent polls
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("event") },
                      { QStringLiteral("event"), obj({ { QStringLiteral("type"), QStringLiteral("future_feature") } }) } }));
    QTest::qWait(700);
    const int polls = requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events")).size();
    QVERIFY2(polls >= 2 && polls <= 6,
             qPrintable(QStringLiteral("expected poll floor to cap polls in 700ms, got %1").arg(polls)));
}

void TestSdsClient::capabilitiesEventsCoalesced()
{
    startClient(false, 1, false);
    QVERIFY(waitReady());
    QTest::qWait(250); // let long poll park

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("clear_log") } }));
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("delay") },
                        { QStringLiteral("method"), QStringLiteral("GET") },
                        { QStringLiteral("path_prefix"), QStringLiteral("/sds/v1/capabilities") },
                        { QStringLiteral("ms"), 150 },
                        { QStringLiteral("count"), 5 } }));
    m_Host->config(obj({ { QStringLiteral("permissions"), strings({ "clipboard_read", "file_download" }) } }));
    QJsonArray burst;
    for (int i = 0; i < 8; ++i) {
        burst.append(obj({ { QStringLiteral("type"), QStringLiteral("capabilities") } }));
    }
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("event") },
                      { QStringLiteral("events"), burst } }));
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::CapabilitiesChanged)));
    QTest::qWait(400);
    const int capReqs = requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/capabilities")).size();
    QVERIFY2(capReqs >= 1 && capReqs <= 2,
             qPrintable(QStringLiteral("expected coalesced capabilities fetches <= 2, got %1").arg(capReqs)));
}

void TestSdsClient::fileClipboardIgnoresNonAdvancingSeqOn200()
{
    const auto bundle1 = encodeFileClipboard({{QStringLiteral("first.txt"), "one"}});
    const auto bundle2 = encodeFileClipboard({{QStringLiteral("second.txt"), "two"}});
    startClient(false, 1, true);
    QVERIFY(waitReady());

    // Baseline seq=0
    m_Sds->exchangeFileClipboard({}, 100);
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::FileClipboardObserved)));

    // Receive seq=1
    m_Host->cmd(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("file_clipboard")},
                            {QStringLiteral("bundle"), QJsonDocument::fromJson(bundle1).object()}});
    m_Events.clear();
    m_Sds->exchangeFileClipboard({}, 101);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return !e.fileBundle.isEmpty(); }));

    // Host spuriously replies 200 with seq == m_FileSeq (1): must be ignored without restaging
    m_Events.clear();
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("stale_file_clip_200") },
                        { QStringLiteral("count"), 1 } }));
    m_Sds->exchangeFileClipboard({}, 102);
    QTest::qWait(250);
    for (const auto& ev : m_Events.events()) {
        QVERIFY(ev.fileBundle.isEmpty());
    }

    // Real newer seq=2 still delivers normally
    m_Host->cmd(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("file_clipboard")},
                            {QStringLiteral("bundle"), QJsonDocument::fromJson(bundle2).object()}});
    m_Sds->exchangeFileClipboard({}, 103);
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) { return !e.fileBundle.isEmpty(); }));
    QList<ClipboardFile> decoded;
    for (const auto& ev : m_Events.events()) {
        if (!ev.fileBundle.isEmpty()) {
            QVERIFY(decodeFileClipboard(ev.fileBundle, &decoded));
        }
    }
    QCOMPARE(decoded.size(), 1);
    QCOMPARE(decoded[0].name, QStringLiteral("second.txt"));
}

void TestSdsClient::uploadRejectsSymlinkAndFifo()
{
#ifdef Q_OS_UNIX
    startClient(false, 1, false);
    QVERIFY(waitReady());

    const QString target = m_Tmp->filePath(QStringLiteral("real.txt"));
    QVERIFY(writeFile(target, "secret"));
    const QString link = m_Tmp->filePath(QStringLiteral("link.txt"));
    QCOMPARE(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()), 0);
    const QString fifo = m_Tmp->filePath(QStringLiteral("pipe.txt"));
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);

    m_Sds->sendFile(link);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("not a regular file"))));
    m_Events.clear();
    m_Sds->sendFile(fifo);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("not a regular file"))));
    QTest::qWait(150);
    QVERIFY(requests(QStringLiteral("POST"), QStringLiteral("/sds/v1/files/uploads")).isEmpty());
#else
    QSKIP("POSIX symlink/FIFO upload test");
#endif
}

void TestSdsClient::hostNamesAreRejectedBeforeAnyRequest()
{
    // localhost would reach the working mock if DNS/network dispatch were attempted.
    SdsClient::Config cfg;
    cfg.host = QStringLiteral("localhost");
    cfg.httpsPort = m_Host->port();
    cfg.serverCert = m_ServerA.cert;
    cfg.clientCert = m_Client.cert;
    cfg.clientKey = m_Client.key;
    cfg.downloadDir = m_Downloads;
    m_Sds = std::make_unique<SdsClient>(cfg, &m_Events);
    m_Sds->start();
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Stopped)));
    QVERIFY(m_Events.hasNoticeContaining(QStringLiteral("private LAN IP")));
    QCOMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/")).size(), 0);
}

bool TestSdsClient::waitReady()
{
    return m_Events.waitFor(EventCollector::ofType(ClientEvent::Ready));
}

QList<QJsonObject> TestSdsClient::requests(const QString& method, const QString& pathPrefix)
{
    QList<QJsonObject> out;
    for (const QJsonObject& e : m_Host->log()) {
        if (e.value(QStringLiteral("method")).toString() == method &&
                e.value(QStringLiteral("path")).toString().startsWith(pathPrefix)) {
            out.append(e);
        }
    }
    return out;
}

QJsonArray TestSdsClient::clientPuts()
{
    return m_Host->state().value(QStringLiteral("client_puts")).toArray();
}

void TestSdsClient::capabilitiesAndReady()
{
    m_Host->config(obj({ { QStringLiteral("permissions"), strings({ "clipboard_read", "file_upload" }) },
                         { QStringLiteral("limits"), obj({ { QStringLiteral("clipboard_image_max"), 1073741824.0 },
                                                           { QStringLiteral("chunk_max"), 65536 } }) } }));
    startClient();
    QVERIFY(waitReady());

    ClientEvent ready;
    for (const ClientEvent& e : m_Events.events()) {
        if (e.type == ClientEvent::Ready) {
            ready = e;
        }
    }
    QCOMPARE(ready.caps.version, 1);
    QCOMPARE(ready.caps.permissions, (QSet<QString>{ QStringLiteral("clipboard_read"), QStringLiteral("file_upload") }));
    QCOMPARE(ready.caps.limits.clipboardImageMax, kClientImageMax); // clamped to the client cap
    QCOMPARE(ready.caps.limits.clipboardTextMax, qint64(1048576));
    QCOMPARE(ready.caps.limits.chunkMax, qint64(65536));
    QVERIFY(!ready.hostProvidedClipboard);

    // capabilities first, then an immediate poll, then long polls
    QVERIFY(QTest::qWaitFor([this]() { return requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events")).size() >= 2; }, 5000));
    QList<QJsonObject> log = m_Host->log();
    QCOMPARE(log.first().value(QStringLiteral("path")).toString(), QStringLiteral("/sds/v1/capabilities"));
    QList<QJsonObject> polls = requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events"));
    QCOMPARE(polls[0].value(QStringLiteral("query")).toObject().value(QStringLiteral("timeout")).toString(), QStringLiteral("0"));
    QCOMPARE(polls[0].value(QStringLiteral("query")).toObject().value(QStringLiteral("after")).toString(), QStringLiteral("0"));
    QCOMPARE(polls[1].value(QStringLiteral("query")).toObject().value(QStringLiteral("timeout")).toString(), QStringLiteral("2"));
}

void TestSdsClient::hostClipboardAtStartWins()
{
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") }, { QStringLiteral("text"), QStringLiteral("hello from host") } }));
    startClient();
    QVERIFY(waitReady());

    bool hostProvided = false;
    for (const ClientEvent& e : m_Events.events()) {
        if (e.type == ClientEvent::Ready) {
            hostProvided = e.hostProvidedClipboard;
        }
    }
    QVERIFY(hostProvided);
    QVERIFY(m_Events.waitFor(clipboardText("hello from host")));
}

void TestSdsClient::longPollDeliversEvents()
{
    startClient();
    QVERIFY(waitReady());
    QTest::qWait(300); // let the long poll park

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("event") },
                      { QStringLiteral("event"), obj({ { QStringLiteral("type"), QStringLiteral("future_feature") } }) } }));
    QByteArray png = SdsTest::makePng(8, 8);
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") },
                      { QStringLiteral("text"), QStringLiteral("both") },
                      { QStringLiteral("png_b64"), QString::fromLatin1(png.toBase64()) } }));

    QVERIFY(m_Events.waitFor([&png](const ClientEvent& e) {
        return e.type == ClientEvent::ClipboardReceived && e.clipboard.text == "both" && e.clipboard.png == png;
    }));

    // Polls advance the cursor past both events (the unknown one is skipped, not fatal)
    QVERIFY(QTest::qWaitFor([&]() {
        for (const QJsonObject& p : requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events"))) {
            if (p.value(QStringLiteral("query")).toObject().value(QStringLiteral("after")).toString().toInt() >= 2) {
                return true;
            }
        }
        return false;
    }, 5000));
}

void TestSdsClient::clipboardGone410()
{
    startClient();
    QVERIFY(waitReady());

    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("clip_410") }, { QStringLiteral("count"), 1 } }));
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") }, { QStringLiteral("text"), QStringLiteral("first") } }));
    QVERIFY(QTest::qWaitFor([this]() { return !requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/clipboard")).isEmpty(); }, 5000));

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") }, { QStringLiteral("text"), QStringLiteral("second") } }));
    QVERIFY(m_Events.waitFor(clipboardText("second")));
    QCOMPARE(m_Events.count(clipboardText("first")), 0);

    QList<QJsonObject> gets = requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/clipboard"));
    QCOMPARE(gets.first().value(QStringLiteral("status")).toInt(), 410);
}

void TestSdsClient::clipboardPutSendsBothRepresentations()
{
    startClient();
    QVERIFY(waitReady());

    QByteArray png = SdsTest::makePng(4, 4);
    ClipboardItem item;
    item.text = "from mac";
    item.png = png;
    m_Sds->sendClipboard(item);

    QVERIFY(QTest::qWaitFor([this]() { return clientPuts().size() == 2; }, 5000));
    QJsonArray puts = clientPuts();
    QJsonObject text = puts[0].toObject();
    QJsonObject image = puts[1].toObject();
    QCOMPARE(text.value(QStringLiteral("type")).toString(), QStringLiteral("text/plain"));
    QCOMPARE(QByteArray::fromBase64(text.value(QStringLiteral("data_b64")).toString().toLatin1()), QByteArray("from mac"));
    QCOMPARE(text.value(QStringLiteral("client_seq")).toString(), QStringLiteral("1"));
    QCOMPARE(text.value(QStringLiteral("content_length")).toInt(), 8);
    QCOMPARE(image.value(QStringLiteral("type")).toString(), QStringLiteral("image/png"));
    QCOMPARE(QByteArray::fromBase64(image.value(QStringLiteral("data_b64")).toString().toLatin1()), png);
    QCOMPARE(image.value(QStringLiteral("client_seq")).toString(), QStringLiteral("1"));

    for (const QJsonObject& r : requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/clipboard"))) {
        QVERIFY(r.value(QStringLiteral("transfer_encoding")).isNull());
    }

    ClipboardItem next;
    next.text = "second";
    m_Sds->sendClipboard(next);
    QVERIFY(QTest::qWaitFor([this]() { return clientPuts().size() == 3; }, 5000));
    QCOMPARE(clientPuts()[2].toObject().value(QStringLiteral("client_seq")).toString(), QStringLiteral("2"));
}

void TestSdsClient::clipboardRejectedClientSide()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("clipboard_text_max"), 16 } }) } }));
    startClient();
    QVERIFY(waitReady());

    ClipboardItem invalid;
    invalid.text = QByteArray("\xFF\xFE bad");
    m_Sds->sendClipboard(invalid);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("not valid UTF-8"))));

    ClipboardItem big;
    big.text = QByteArray(17, 'a');
    m_Sds->sendClipboard(big);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("text too large"))));

    ClipboardItem notPng;
    notPng.png = QByteArray("GIF89a definitely not a png image");
    m_Sds->sendClipboard(notPng);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("not a valid PNG"))));

    QTest::qWait(300);
    QVERIFY(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/clipboard")).isEmpty());
}

void TestSdsClient::hostClipboardValidated()
{
    startClient();
    QVERIFY(waitReady());

    // NUL inside text
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") },
                      { QStringLiteral("text_b64"), QString::fromLatin1(QByteArray("bad\0text", 8).toBase64()) } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("Rejected host clipboard"))));

    // Announced size above the limit: not even fetched
    int getsBefore = requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/clipboard")).size();
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") },
                      { QStringLiteral("text"), QStringLiteral("small") },
                      { QStringLiteral("announce_sizes"), obj({ { QStringLiteral("text/plain"), 2000000 } }) } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("Host clipboard text too large"))));
    QCOMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/clipboard")).size(), getsBefore);

    // Bad PNG header
    QByteArray badPng = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(40, 'z');
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") },
                      { QStringLiteral("png_b64"), QString::fromLatin1(badPng.toBase64()) } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("not a valid PNG"))));

    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::ClipboardReceived)), 0);
}

void TestSdsClient::clipboardErrorCodes()
{
    startClient();
    QVERIFY(waitReady());

    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("status") }, { QStringLiteral("method"), QStringLiteral("PUT") },
                        { QStringLiteral("path_prefix"), QStringLiteral("/sds/v1/clipboard") }, { QStringLiteral("status"), 403 },
                        { QStringLiteral("error"), QStringLiteral("forbidden") } }));
    ClipboardItem x;
    x.text = "x";
    m_Sds->sendClipboard(x);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("does not allow setting its clipboard"))));

    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("status") }, { QStringLiteral("method"), QStringLiteral("PUT") },
                        { QStringLiteral("path_prefix"), QStringLiteral("/sds/v1/clipboard") }, { QStringLiteral("status"), 413 },
                        { QStringLiteral("error"), QStringLiteral("too_large") } }));
    ClipboardItem y;
    y.text = "y";
    m_Sds->sendClipboard(y);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("too large for the host"))));

    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("status") }, { QStringLiteral("method"), QStringLiteral("PUT") },
                        { QStringLiteral("path_prefix"), QStringLiteral("/sds/v1/clipboard") }, { QStringLiteral("status"), 429 },
                        { QStringLiteral("error"), QStringLiteral("rate_limited") }, { QStringLiteral("retry_after"), 100 } }));
    ClipboardItem z;
    z.text = "z";
    QElapsedTimer t;
    t.start();
    m_Sds->sendClipboard(z);
    QVERIFY(QTest::qWaitFor([this]() {
        const QJsonArray puts = clientPuts();
        return !puts.isEmpty() && QByteArray::fromBase64(puts.last().toObject().value(QStringLiteral("data_b64")).toString().toLatin1()) == "z";
    }, 8000));
    QVERIFY2(t.elapsed() >= 900, "Retry-After was not honoured");
    QVERIFY2(t.elapsed() < 5000, "Retry-After was not capped");

    QList<QJsonObject> puts = requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/clipboard"));
    QCOMPARE(puts.size(), 4); // 403, 413, 429, then the retried z
    QCOMPARE(puts[2].value(QStringLiteral("status")).toInt(), 429);
    QCOMPARE(puts[3].value(QStringLiteral("status")).toInt(), 204);
}

void TestSdsClient::uploadResumesAfterConflict()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("chunk_max"), 65536 } }) } }));
    // 2nd chunk: accepted but the response is lost. 4th chunk request: host "loses" the
    // previous chunk and asks to resend from an earlier offset.
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("drop_after_accept") }, { QStringLiteral("skip"), 1 } }));
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("rewind") }, { QStringLiteral("skip"), 3 } }));

    startClient();
    QVERIFY(waitReady());

    QByteArray data = SdsTest::randomBytes(300000);
    QString path = m_Tmp->filePath(QStringLiteral("big data.bin"));
    QVERIFY(writeFile(path, data));
    m_Sds->sendFile(path);

    QVERIFY(m_Events.waitFor(transfer(true, true), 20000));
    QCOMPARE(m_Events.count(transfer(true, false)), 0);

    QJsonArray completed = m_Host->state().value(QStringLiteral("completed")).toArray();
    QCOMPARE(completed.size(), 1);
    QJsonObject c = completed[0].toObject();
    QCOMPARE(c.value(QStringLiteral("name")).toString(), QStringLiteral("big data.bin"));
    QCOMPARE(c.value(QStringLiteral("sha256")).toString(), sha256Hex(data));
    QCOMPARE(QByteArray::fromBase64(c.value(QStringLiteral("data_b64")).toString().toLatin1()), data);

    QList<QJsonObject> puts = requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/files/uploads/"));
    bool sawDrop = false, saw409 = false;
    for (const QJsonObject& p : puts) {
        QVERIFY(p.value(QStringLiteral("length")).toInt() <= 65536);
        QVERIFY(p.value(QStringLiteral("transfer_encoding")).isNull());
        sawDrop = sawDrop || p.value(QStringLiteral("status")).toString() == QLatin1String("dropped");
        saw409 = saw409 || p.value(QStringLiteral("status")).toInt() == 409;
    }
    QVERIFY(sawDrop);
    QVERIFY(saw409);
    QVERIFY(puts.size() >= 7); // 5 chunks + retry + resend

    QVERIFY(m_Events.count([](const ClientEvent& e) {
        return e.type == ClientEvent::TransferProgress && e.upload && e.percent == 100;
    }) > 0);
}

void TestSdsClient::uploadNamesAndCollisions()
{
    startClient();
    QVERIFY(waitReady());

    QString path = m_Tmp->filePath(QStringLiteral("..evil:name?.txt"));
    QVERIFY(writeFile(path, "payload"));

    m_Sds->sendFile(path);
    QVERIFY(m_Events.waitFor(transfer(true, true)));
    m_Sds->sendFile(path);
    QVERIFY(QTest::qWaitFor([this]() { return m_Events.count(transfer(true, true)) == 2; }, 10000));

    QJsonArray completed = m_Host->state().value(QStringLiteral("completed")).toArray();
    QCOMPARE(completed.size(), 2);
    QCOMPARE(completed[0].toObject().value(QStringLiteral("requested_name")).toString(), QStringLiteral("_.evil_name_.txt"));
    QCOMPARE(completed[1].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("_.evil_name_ (1).txt"));

    QStringList names;
    for (const ClientEvent& e : m_Events.events()) {
        if (e.type == ClientEvent::TransferFinished && e.ok) {
            names.append(e.name);
        }
    }
    QCOMPARE(names, QStringList() << QStringLiteral("_.evil_name_.txt") << QStringLiteral("_.evil_name_ (1).txt"));
}

void TestSdsClient::uploadRefusalsAndCapabilitiesEvent()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("file_max"), 1000 } }) } }));
    startClient();
    QVERIFY(waitReady());

    m_Sds->sendFile(m_Tmp->path());
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("not a regular file"))));

    QString big = m_Tmp->filePath(QStringLiteral("big.bin"));
    QVERIFY(writeFile(big, QByteArray(2000, 'b')));
    m_Sds->sendFile(big);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("too large to send"))));

    // The admin revokes file_upload mid-stream and the host announces it
    m_Host->config(obj({ { QStringLiteral("permissions"), strings({ "clipboard_read", "clipboard_write", "file_download" }) } }));
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("event") },
                      { QStringLiteral("event"), obj({ { QStringLiteral("type"), QStringLiteral("capabilities") } }) } }));
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::CapabilitiesChanged)));

    QString small = m_Tmp->filePath(QStringLiteral("small.txt"));
    QVERIFY(writeFile(small, "ok"));
    m_Sds->sendFile(small);
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("does not allow sending files"))));

    QTest::qWait(200);
    QVERIFY(requests(QStringLiteral("POST"), QStringLiteral("/sds/v1/files/uploads")).isEmpty());
}

void TestSdsClient::uploadVerificationFailure()
{
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("verify_fail") } }));
    startClient();
    QVERIFY(waitReady());

    QString path = m_Tmp->filePath(QStringLiteral("doc.txt"));
    QVERIFY(writeFile(path, "content"));
    m_Sds->sendFile(path);
    QVERIFY(m_Events.waitFor(transfer(true, false)));
    QVERIFY(m_Events.hasNoticeContaining(QStringLiteral("verification failed")));
}

void TestSdsClient::uploadEmptyFile()
{
    startClient();
    QVERIFY(waitReady());

    QString path = m_Tmp->filePath(QStringLiteral("empty.txt"));
    QVERIFY(writeFile(path, QByteArray()));
    m_Sds->sendFile(path);
    QVERIFY(m_Events.waitFor(transfer(true, true)));

    QJsonArray completed = m_Host->state().value(QStringLiteral("completed")).toArray();
    QCOMPARE(completed.size(), 1);
    QCOMPARE(completed[0].toObject().value(QStringLiteral("size")).toInt(), 0);
    QVERIFY(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/files/uploads/")).isEmpty());
}

void TestSdsClient::outboxDownloadsAndSanitizes()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("chunk_max"), 65536 } }) } }));
    startClient();
    QVERIFY(waitReady());

    QByteArray photo = SdsTest::randomBytes(200000);
    QByteArray script("#!/bin/sh\necho owned\n");
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("photo.jpg") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(photo.toBase64()) } }));
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("../../evil.sh") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(script.toBase64()) } }));

    QVERIFY(QTest::qWaitFor([this]() { return m_Events.count(transfer(false, true)) == 2; }, 20000));

    const QString evilName = sanitizeFileName(QStringLiteral("../../evil.sh"));
    QCOMPARE(evilName, QStringLiteral("_._.._evil.sh"));
    QString photoPath = QDir(m_Downloads).filePath(QStringLiteral("photo.jpg"));
    QString evilPath = QDir(m_Downloads).filePath(evilName);
    QCOMPARE(readFile(photoPath), photo);
    QCOMPARE(readFile(evilPath), script);

    for (const QString& p : { photoPath, evilPath }) {
        QVERIFY2(hasQuarantine(p), qPrintable(p));
        QVERIFY2(!isExecutable(p), qPrintable(p));
    }

    // Nothing outside the download folder, no part files left behind
    QCOMPARE(QDir(m_Tmp->path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
             QStringList() << QStringLiteral("Downloads"));
    QCOMPARE(QDir(m_Tmp->filePath(QStringLiteral("Downloads"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
             QStringList() << QStringLiteral("Moonlight"));
    QStringList files = QDir(m_Downloads).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    QCOMPARE(files, QStringList() << evilName << QStringLiteral("photo.jpg"));

    // Ranged requests within chunk_max
    int ranges = 0;
    for (const QJsonObject& r : requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/files/outbox/"))) {
        QVERIFY(r.value(QStringLiteral("query")).toObject().value(QStringLiteral("length")).toString().toInt() <= 65536);
        ranges++;
    }
    QCOMPARE(ranges, 4 + 1); // 200000 bytes in 64 KiB ranges + the small script

    for (const ClientEvent& e : m_Events.events()) {
        if (e.type == ClientEvent::TransferFinished && e.ok) {
            QVERIFY(e.path.startsWith(m_Downloads + QLatin1Char('/')));
        }
    }
}

void TestSdsClient::outboxCollisionAndDedupe()
{
    QVERIFY(QDir().mkpath(m_Downloads));
    QString local = QDir(m_Downloads).filePath(QStringLiteral("report.pdf"));
    QVERIFY(writeFile(local, "local copy"));

    startClient();
    QVERIFY(waitReady());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("report.pdf") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("remote copy").toBase64()) } }));
    QVERIFY(m_Events.waitFor(transfer(false, true)));
    QCOMPARE(readFile(local), QByteArray("local copy"));
    QCOMPARE(readFile(QDir(m_Downloads).filePath(QStringLiteral("report (1).pdf"))), QByteArray("remote copy"));

    // A later outbox event only fetches files not downloaded in this session
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("other.txt") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("other").toBase64()) } }));
    QVERIFY(QTest::qWaitFor([this]() { return m_Events.count(transfer(false, true)) == 2; }, 10000));
    QTest::qWait(500);
    QCOMPARE(m_Events.count(transfer(false, true)), 2);
    QCOMPARE(QDir(m_Downloads).entryList(QDir::Files | QDir::Hidden, QDir::Name),
             QStringList() << QStringLiteral("other.txt") << QStringLiteral("report (1).pdf") << QStringLiteral("report.pdf"));
}

void TestSdsClient::outboxIntegrityFailures()
{
    startClient();
    QVERIFY(waitReady());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("tampered.bin") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("abcdef").toBase64()) },
                      { QStringLiteral("bad_sha"), true } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("integrity check"))));

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("greedy.bin") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("abcdef").toBase64()) },
                      { QStringLiteral("extra_bytes"), 100000 } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("more data than requested"))));

    QCOMPARE(m_Events.count(transfer(false, true)), 0);
    QCOMPARE(QDir(m_Downloads).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList());
}

void TestSdsClient::outboxManualDownload()
{
    startClient(false);
    QVERIFY(waitReady());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("a.txt") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("aaa").toBase64()) } }));
    QTest::qWait(1000);
    QCOMPARE(m_Events.count(transfer(false, true)), 0);
    QVERIFY(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/files/outbox")).isEmpty());

    m_Sds->downloadOutboxNow();
    QVERIFY(m_Events.waitFor(transfer(false, true)));
    QCOMPARE(readFile(QDir(m_Downloads).filePath(QStringLiteral("a.txt"))), QByteArray("aaa"));

    m_Sds->downloadOutboxNow();
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("No new files"))));
}

void TestSdsClient::featureDisabledStopsQuietly()
{
    m_Host->config(obj({ { QStringLiteral("feature_enabled"), false } }));
    startClient();
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Stopped)));
    QVERIFY(m_Events.hasNoticeContaining(QStringLiteral("disabled on the host"))); // in the Stopped event
    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::Notice)), 0);      // but not shown
    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::Ready)), 0);
}

void TestSdsClient::noActiveStreamThenActive()
{
    m_Host->config(obj({ { QStringLiteral("stream_active"), false } }));
    startClient();
    QVERIFY(QTest::qWaitFor([this]() { return requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/capabilities")).size() >= 3; }, 5000));
    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::Ready)), 0);
    for (const QJsonObject& r : requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/capabilities"))) {
        // The newest entry may still be in flight (status not yet recorded)
        if (!r.value(QStringLiteral("status")).isNull()) {
            QCOMPARE(r.value(QStringLiteral("status")).toInt(), 409);
        }
    }

    m_Host->config(obj({ { QStringLiteral("stream_active"), true } }));
    QVERIFY(waitReady());
}

void TestSdsClient::rateLimitedCapabilities()
{
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("status") }, { QStringLiteral("method"), QStringLiteral("GET") },
                        { QStringLiteral("path_prefix"), QStringLiteral("/sds/v1/capabilities") }, { QStringLiteral("status"), 429 },
                        { QStringLiteral("error"), QStringLiteral("rate_limited") }, { QStringLiteral("retry_after"), 100 } }));
    QElapsedTimer t;
    t.start();
    startClient();
    QVERIFY(waitReady());
    QVERIFY(t.elapsed() >= 900);  // honoured...
    QVERIFY(t.elapsed() < 5000);  // ...but capped (1 s in the test configuration)
    QCOMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/capabilities")).size(), 2);
}

void TestSdsClient::streamEndedStopsClient()
{
    startClient();
    QVERIFY(waitReady());
    QTest::qWait(200);

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("end_stream") } }));
    QVERIFY(m_Events.waitFor([](const ClientEvent& e) {
        return e.type == ClientEvent::Stopped && e.message == QLatin1String("Stream ended");
    }));

    int polls = requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events")).size();
    ClipboardItem item;
    item.text = "after the stream";
    m_Sds->sendClipboard(item);
    QTest::qWait(1500);
    QCOMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events")).size(), polls);
    QVERIFY(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/clipboard")).isEmpty());
}

void TestSdsClient::pinningRejectsWrongServerCert()
{
    // The host presents a different certificate than the one pinned at pairing
    QVERIFY(startMock(m_ServerB, m_Client));
    startClient();

    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("certificate does not match"))));
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Stopped)));
    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::Ready)), 0);

    QJsonObject log = m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("log") } }));
    // No HTTP request reached the host. (Whether the host saw a failed handshake or a closed
    // connection after it depends on the client's TLS backend.)
    QCOMPARE(log.value(QStringLiteral("log")).toArray().size(), 0);
}

void TestSdsClient::hostRejectsUnknownClient()
{
    // The host only trusts another client certificate: TLS fails, the client keeps retrying
    // with backoff and never reports a pin failure (the server certificate is fine).
    QVERIFY(startMock(m_ServerA, m_OtherClient));
    startClient();
    QTest::qWait(2000);
    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::Ready)), 0);
    QVERIFY(!m_Events.hasNoticeContaining(QStringLiteral("certificate does not match")));
    QJsonObject log = m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("log") } }));
    QCOMPARE(log.value(QStringLiteral("log")).toArray().size(), 0);
    QVERIFY(log.value(QStringLiteral("handshake_failures")).toInt() >= 1);
}

void TestSdsClient::stopIsPromptDuringLongPoll()
{
    startClient(true, 30);
    QVERIFY(waitReady());
    QTest::qWait(500); // 30 s long poll parked on the host

    QElapsedTimer t;
    t.start();
    m_Sds->stop();
    QVERIFY2(t.elapsed() < 1000, qPrintable(QStringLiteral("stop() took %1 ms").arg(t.elapsed())));

    int events = m_Events.events().size();
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") }, { QStringLiteral("text"), QStringLiteral("late") } }));
    QTest::qWait(500);
    QCOMPARE(m_Events.events().size(), events);
    m_Sds.reset();
}

void TestSdsClient::outboxRangeRetry()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("chunk_max"), 65536 } }) } }));
    // The second range is cut off half way: the client re-requests that range
    m_Host->fault(obj({ { QStringLiteral("kind"), QStringLiteral("drop_range") }, { QStringLiteral("skip"), 1 } }));
    startClient();
    QVERIFY(waitReady());

    QByteArray data = SdsTest::randomBytes(200000);
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("video.mp4") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(data.toBase64()) } }));
    QVERIFY(m_Events.waitFor(transfer(false, true), 20000));
    QCOMPARE(readFile(QDir(m_Downloads).filePath(QStringLiteral("video.mp4"))), data);

    int atSecondRange = 0;
    bool sawDrop = false;
    for (const QJsonObject& r : requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/files/outbox/"))) {
        if (r.value(QStringLiteral("query")).toObject().value(QStringLiteral("offset")).toString() == QLatin1String("65536")) {
            atSecondRange++;
        }
        sawDrop = sawDrop || r.value(QStringLiteral("status")).toString() == QLatin1String("dropped");
    }
    QVERIFY(sawDrop);
    QCOMPARE(atSecondRange, 2);
    QCOMPARE(QDir(m_Downloads).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
             QStringList() << QStringLiteral("video.mp4"));
}

void TestSdsClient::outboxRequiresChecksumAndSize()
{
    startClient();
    QVERIFY(waitReady());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("unsigned.bin") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("data").toBase64()) },
                      { QStringLiteral("no_sha"), true } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("without a checksum"))));

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("resized.bin") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("data").toBase64()) },
                      { QStringLiteral("header_size"), 999999 } }));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("changed on the host"))));

    QCOMPARE(m_Events.count(transfer(false, true)), 0);
    QCOMPARE(QDir(m_Downloads).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList());
}

void TestSdsClient::outboxIgnoresBadIdsAndHandlesEmptyFile()
{
    startClient();
    QVERIFY(waitReady());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("trap.txt") },
                      { QStringLiteral("id"), QStringLiteral("../../../etc/passwd") },
                      { QStringLiteral("data_b64"), QString::fromLatin1(QByteArray("x").toBase64()) } }));
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("outbox_add") }, { QStringLiteral("name"), QStringLiteral("empty.txt") },
                      { QStringLiteral("data_b64"), QString() } }));

    QVERIFY(m_Events.waitFor(transfer(false, true)));
    QString empty = QDir(m_Downloads).filePath(QStringLiteral("empty.txt"));
    QVERIFY(QFile::exists(empty));
    QCOMPARE(QFileInfo(empty).size(), qint64(0));
    QVERIFY(hasQuarantine(empty));

    QTest::qWait(500);
    for (const QJsonObject& r : requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/files/outbox/"))) {
        QVERIFY(!r.value(QStringLiteral("path")).toString().contains(QStringLiteral("etc")));
    }
    QCOMPARE(m_Events.count(transfer(false, true)), 1);
}

void TestSdsClient::unsupportedVersion()
{
    m_Host->config(obj({ { QStringLiteral("version"), 2 } }));
    startClient();
    QVERIFY(m_Events.waitFor(EventCollector::ofType(ClientEvent::Stopped)));
    QVERIFY(m_Events.waitFor(EventCollector::noticeContaining(QStringLiteral("Unsupported"))));
    QCOMPARE(m_Events.count(EventCollector::ofType(ClientEvent::Ready)), 0);
    QVERIFY(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events")).isEmpty());
}

void TestSdsClient::eventNumberingReset()
{
    startClient();
    QVERIFY(waitReady());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") }, { QStringLiteral("text"), QStringLiteral("one") } }));
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("event") },
                      { QStringLiteral("event"), obj({ { QStringLiteral("type"), QStringLiteral("future_feature") } }) } }));
    QVERIFY(m_Events.waitFor(clipboardText("one")));
    QVERIFY(QTest::qWaitFor([this]() {
        for (const QJsonObject& p : requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/events"))) {
            if (p.value(QStringLiteral("query")).toObject().value(QStringLiteral("after")).toString() == QLatin1String("2")) {
                return true;
            }
        }
        return false;
    }, 5000));

    // Host restarts numbering at 1; the client notices last_id < after and starts over
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("reset_events") } }));
    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("host_clipboard") }, { QStringLiteral("text"), QStringLiteral("two") } }));
    QVERIFY(m_Events.waitFor(clipboardText("two")));
}

void TestSdsClient::lazyClipboardClientOfferAndChunkedPull()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("chunk_max"), 1024 } }) } }));
    startClient(true, 2, true);
    QVERIFY(waitReady());

    const QByteArray payload = SdsTest::randomBytes(3000);
    const QString srcPath = QFileInfo(m_Tmp->path()).canonicalFilePath() + QStringLiteral("/payload.bin");
    QVERIFY(writeFile(srcPath, payload));

    LazyClipboardOffer offer;
    offer.offerId = QStringLiteral("c_lazy_1");
    offer.kind = QStringLiteral("files");
    offer.entries.append({QStringLiteral("e0"), QStringLiteral("payload.bin"), payload.size(), srcPath});
    m_Sds->publishLazyClipboardOffer(offer);

    QVERIFY(QTest::qWaitFor([this]() {
        return !m_Host->state().value(QStringLiteral("client_offers")).toArray().isEmpty();
    }, 5000));
    QVERIFY(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/clipboard-offer/c_lazy_1/entries/")).isEmpty());

    m_Host->cmd(obj({ { QStringLiteral("cmd"), QStringLiteral("emit_pull") },
                      { QStringLiteral("offer_id"), QStringLiteral("c_lazy_1") },
                      { QStringLiteral("targets"), strings({"text/uri-list"}) } }));
    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::LazyClipboardPullRequested && ev.offerId == QLatin1String("c_lazy_1");
    }));

    m_Sds->fulfillLazyClipboardPull(offer);
    QVERIFY(QTest::qWaitFor([this]() {
        return m_Host->state().value(QStringLiteral("completed_offers")).toObject().contains(QStringLiteral("c_lazy_1"));
    }, 5000));

    const QString gotB64 = m_Host->state().value(QStringLiteral("completed_offers")).toObject()
            .value(QStringLiteral("c_lazy_1")).toObject()
            .value(QStringLiteral("payload.bin")).toString();
    QCOMPARE(QByteArray::fromBase64(gotB64.toLatin1()), payload);
    QCOMPARE(requests(QStringLiteral("PUT"), QStringLiteral("/sds/v1/clipboard-offer/c_lazy_1/entries/e0")).size(), 3);
}

void TestSdsClient::lazyClipboardHostOfferAndDemandFetch()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("chunk_max"), 1024 } }) } }));
    startClient(true, 2, true);
    QVERIFY(waitReady());

    const QByteArray hostBytes = SdsTest::randomBytes(2500);
    m_Host->cmd(obj({
        { QStringLiteral("cmd"), QStringLiteral("host_lazy_offer") },
        { QStringLiteral("offer_id"), QStringLiteral("h_lazy_1") },
        { QStringLiteral("kind"), QStringLiteral("files") },
        { QStringLiteral("entries"), QJsonArray{
            obj({ { QStringLiteral("id"), QStringLiteral("e0") }, { QStringLiteral("name"), QStringLiteral("big.bin") },
                  { QStringLiteral("data_b64"), QString::fromLatin1(hostBytes.toBase64()) } }),
            obj({ { QStringLiteral("id"), QStringLiteral("e1") }, { QStringLiteral("name"), QStringLiteral("empty.txt") },
                  { QStringLiteral("data_b64"), QString() } })
        } }
    }));

    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::LazyClipboardOfferReceived && ev.lazyOffer.offerId == QLatin1String("h_lazy_1");
    }));
    QTest::qWait(200);
    QVERIFY(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/clipboard-offer/")).isEmpty());

    LazyClipboardOffer hostOffer;
    for (const ClientEvent& ev : m_Events.events()) {
        if (ev.type == ClientEvent::LazyClipboardOfferReceived && ev.lazyOffer.offerId == QLatin1String("h_lazy_1")) {
            hostOffer = ev.lazyOffer;
        }
    }

    const QString stageDir = QFileInfo(m_Tmp->path()).canonicalFilePath() + QStringLiteral("/lazy-stage");
    auto result = std::make_shared<LazyTransferResult>();
    m_Sds->fetchLazyClipboardOffer(hostOffer, stageDir, result);

    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::LazyClipboardFinished && ev.offerId == QLatin1String("h_lazy_1");
    }));

    QVERIFY(result->finished);
    QVERIFY(result->ok);
    QCOMPARE(result->stagedPaths.size(), 2);
    QCOMPARE(readFile(result->stagedPaths[0]), hostBytes);
    QCOMPARE(readFile(result->stagedPaths[1]), QByteArray());
    QVERIFY(!isExecutable(result->stagedPaths[0]));
    QVERIFY(hasQuarantine(result->stagedPaths[0]));
    QCOMPARE(requests(QStringLiteral("GET"), QStringLiteral("/sds/v1/clipboard-offer/h_lazy_1/entries/e0")).size(), 3);

    // Also test lazy PNG offer
    const QByteArray png = SdsTest::makePng(4, 4);
    m_Host->cmd(obj({
        { QStringLiteral("cmd"), QStringLiteral("host_lazy_offer") },
        { QStringLiteral("offer_id"), QStringLiteral("h_png_1") },
        { QStringLiteral("kind"), QStringLiteral("image") },
        { QStringLiteral("mime"), QStringLiteral("image/png") },
        { QStringLiteral("entries"), QJsonArray{
            obj({ { QStringLiteral("id"), QStringLiteral("img0") }, { QStringLiteral("name"), QStringLiteral("clipboard.png") },
                  { QStringLiteral("data_b64"), QString::fromLatin1(png.toBase64()) } })
        } }
    }));
    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::LazyClipboardOfferReceived && ev.lazyOffer.offerId == QLatin1String("h_png_1");
    }));
    LazyClipboardOffer pngOffer;
    for (const ClientEvent& ev : m_Events.events()) {
        if (ev.type == ClientEvent::LazyClipboardOfferReceived && ev.lazyOffer.offerId == QLatin1String("h_png_1")) {
            pngOffer = ev.lazyOffer;
        }
    }
    auto pngRes = std::make_shared<LazyTransferResult>();
    m_Sds->fetchLazyClipboardOffer(pngOffer, QString(), pngRes);
    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::LazyClipboardFinished && ev.offerId == QLatin1String("h_png_1");
    }));
    QVERIFY(pngRes->ok);
    QCOMPARE(pngRes->png, png);
}

void TestSdsClient::unifiedDropsMultiFileAndFallback()
{
    m_Host->config(obj({ { QStringLiteral("limits"), obj({ { QStringLiteral("chunk_max"), 1024 } }) },
                         { QStringLiteral("drop_delivered"), QStringLiteral("xdnd") } }));
    startClient(true, 2, false, true);
    QVERIFY(waitReady());

    const QString root = QFileInfo(m_Tmp->path()).canonicalFilePath();
    const QByteArray data1 = SdsTest::randomBytes(2500);
    const QByteArray data2 = QByteArray("small-drop");
    const QString f1 = root + QStringLiteral("/chunked.bin");
    const QString f2 = root + QStringLiteral("/note.txt");
    QVERIFY(writeFile(f1, data1));
    QVERIFY(writeFile(f2, data2));

    m_Sds->sendDroppedFiles(0.25, 0.75, {f1, f2});
    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::TransferFinished && ev.upload && ev.ok &&
               ev.message == QLatin1String("Dropped 2 files");
    }));

    const QJsonArray completedDrops = m_Host->state().value(QStringLiteral("completed_drops")).toArray();
    QCOMPARE(completedDrops.size(), 1);
    const QJsonObject drop0 = completedDrops[0].toObject();
    QVERIFY(qAbs(drop0.value(QStringLiteral("nx")).toDouble() - 0.25) < 1e-6);
    QVERIFY(qAbs(drop0.value(QStringLiteral("ny")).toDouble() - 0.75) < 1e-6);
    QCOMPARE(drop0.value(QStringLiteral("files")).toArray().size(), 2);

    // Inbox fallback response produces Downloads/Moonlight status message
    m_Events.clear();
    m_Host->config(obj({ { QStringLiteral("drop_delivered"), QStringLiteral("inbox") } }));
    m_Sds->sendDroppedFiles(0.5, 0.5, {f2});
    QVERIFY(m_Events.waitFor([](const ClientEvent& ev) {
        return ev.type == ClientEvent::TransferFinished && ev.upload && ev.ok &&
               ev.message == QLatin1String("Saved note.txt to ~/Downloads/Moonlight");
    }));
}

void TestSdsClient::tileRefinementLongPollOverPinnedMtls()
{
    std::vector<std::uint8_t> rgb(64 * 64 * 3, 28);
    rgb[(10 * 64 + 10) * 3 + 0] = 255;
    rgb[(10 * 64 + 10) * 3 + 1] = 24;
    rgb[(10 * 64 + 10) * 3 + 2] = 48;
    std::vector<std::uint8_t> qoi;
    QVERIFY(encodeQoiRgb24(rgb.data(), 64 * 3, 64, 64, qoi));

    auto appendU16 = [](QByteArray& dst, std::uint16_t v) {
        dst.append(static_cast<char>(v & 0xFFu));
        dst.append(static_cast<char>((v >> 8) & 0xFFu));
    };
    auto appendU32 = [](QByteArray& dst, std::uint32_t v) {
        dst.append(static_cast<char>(v & 0xFFu));
        dst.append(static_cast<char>((v >> 8) & 0xFFu));
        dst.append(static_cast<char>((v >> 16) & 0xFFu));
        dst.append(static_cast<char>((v >> 24) & 0xFFu));
    };

    QByteArray batchBin;
    batchBin.append("SDTR", 4);
    appendU16(batchBin, AdaptiveRefinementWire::PROTOCOL_VERSION);
    appendU16(batchBin, AdaptiveRefinementWire::FLAG_RESET_ALL);
    appendU16(batchBin, 1920);
    appendU16(batchBin, 1080);
    appendU16(batchBin, 1);
    appendU16(batchBin, 0);
    appendU32(batchBin, 1);
    appendU16(batchBin, 128);
    appendU16(batchBin, 64);
    appendU16(batchBin, 64);
    appendU16(batchBin, 64);
    appendU32(batchBin, static_cast<std::uint32_t>(qoi.size()));
    batchBin.append(reinterpret_cast<const char*>(qoi.data()), static_cast<qsizetype>(qoi.size()));

    QSslServer server;
    QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
    sslCfg.setLocalCertificate(m_ServerA.cert);
    sslCfg.setPrivateKey(m_ServerA.key);
    sslCfg.setCaCertificates({m_Client.cert});
    sslCfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
    server.setSslConfiguration(sslCfg);
    QObject::connect(&server, &QSslServer::sslErrors, &server, [&](QSslSocket* sock, const QList<QSslError>& errors) {
        for (const QSslError& e : errors) {
            if (e.error() == QSslError::SelfSignedCertificate || e.error() == QSslError::SelfSignedCertificateInChain) {
                continue;
            }
            return;
        }
        sock->ignoreSslErrors();
    });

    int pollCount = 0;
    QStringList seenPaths;
    QSslCertificate seenPeerCert;
    QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&]() {
        while (QTcpSocket* rawSock = server.nextPendingConnection()) {
            auto* sslSock = qobject_cast<QSslSocket*>(rawSock);
            QObject::connect(rawSock, &QTcpSocket::readyRead, rawSock, [&, rawSock, sslSock]() {
                if (rawSock->property("replied").toBool()) {
                    return;
                }
                const QByteArray req = rawSock->property("req").toByteArray() + rawSock->readAll();
                if (!req.contains("\r\n\r\n")) {
                    rawSock->setProperty("req", req);
                    return;
                }
                rawSock->setProperty("replied", true);
                if (sslSock) {
                    seenPeerCert = sslSock->peerCertificate();
                }
                const QByteArray firstLine = req.left(req.indexOf("\r\n"));
                seenPaths.append(QString::fromUtf8(firstLine));
                ++pollCount;
                if (pollCount == 1) {
                    rawSock->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else if (pollCount == 2) {
                    QByteArray hdr = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: ") +
                                     QByteArray::number(batchBin.size()) +
                                     QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
                    rawSock->write(hdr);
                    rawSock->write(batchBin);
                } else {
                    rawSock->write("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                }
                rawSock->disconnectFromHost();
            });
            QObject::connect(rawSock, &QTcpSocket::disconnected, rawSock, &QObject::deleteLater);
        }
    });
    QVERIFY(server.listen(QHostAddress::LocalHost));

    // 1. Pinned server + valid paired client identity receives 204 -> 200 batch cleanly
    auto inbox = std::make_shared<AdaptiveTileInbox>();
    SdsClient::Config cfg;
    cfg.host = QStringLiteral("127.0.0.1");
    cfg.httpsPort = server.serverPort();
    cfg.serverCert = m_ServerA.cert;
    cfg.clientCert = m_Client.cert;
    cfg.clientKey = m_Client.key;
    cfg.longPollTimeoutSec = 2;
    cfg.retryBaseMs = 100;
    cfg.retryMaxMs = 500;

    {
        SdsTileRefinementClient client(cfg, inbox);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(inbox->queuedTileCount(), std::size_t(1), 5000);
        client.stop();
    }

    QVERIFY(pollCount >= 2);
    QCOMPARE(seenPeerCert, m_Client.cert);
    QVERIFY(seenPaths[0].startsWith(QStringLiteral("GET /sds/v1/tile-refinement?timeout=2")));

    bool resetAll = false;
    std::uint16_t fw = 0, fh = 0;
    std::vector<AdaptiveDecodedTile> outTiles;
    QVERIFY(inbox->takePending(resetAll, fw, fh, outTiles, RefinementColorspace::Rec601, true));
    QVERIFY(resetAll);
    QCOMPARE(fw, std::uint16_t(1920));
    QCOMPARE(fh, std::uint16_t(1080));
    QCOMPARE(outTiles.size(), std::size_t(1));
    QCOMPARE(outTiles[0].x, std::uint16_t(128));
    QCOMPARE(outTiles[0].y, std::uint16_t(64));
    const auto* rgba = reinterpret_cast<const std::uint8_t*>(outTiles[0].rgba.constData());
    QCOMPARE(rgba[(10 * 64 + 10) * 4 + 0], std::uint8_t(255));
    QCOMPARE(rgba[(10 * 64 + 10) * 4 + 1], std::uint8_t(24));
    QCOMPARE(rgba[(10 * 64 + 10) * 4 + 2], std::uint8_t(48));

    // 2. Wrong pinned server cert must NOT receive any HTTP request or deliver any tiles
    server.close();
    int badHttpRequests = 0;
    QSslServer badServer;
    badServer.setSslConfiguration(sslCfg);
    QObject::connect(&badServer, &QSslServer::sslErrors, &badServer, [&](QSslSocket* sock, const QList<QSslError>& errors) {
        for (const QSslError& e : errors) {
            if (e.error() == QSslError::SelfSignedCertificate || e.error() == QSslError::SelfSignedCertificateInChain) {
                continue;
            }
            return;
        }
        sock->ignoreSslErrors();
    });
    QObject::connect(&badServer, &QSslServer::pendingConnectionAvailable, &badServer, [&]() {
        while (QTcpSocket* rawSock = badServer.nextPendingConnection()) {
            QObject::connect(rawSock, &QTcpSocket::readyRead, rawSock, [&]() {
                ++badHttpRequests;
            });
            QObject::connect(rawSock, &QTcpSocket::disconnected, rawSock, &QObject::deleteLater);
        }
    });
    QVERIFY(badServer.listen(QHostAddress::LocalHost));

    auto badInbox = std::make_shared<AdaptiveTileInbox>();
    cfg.httpsPort = badServer.serverPort();
    cfg.serverCert = m_ServerB.cert;
    {
        SdsTileRefinementClient client(cfg, badInbox);
        client.start();
        QTest::qWait(400);
        client.stop();
    }
    QCOMPARE(badInbox->queuedTileCount(), std::size_t(0));
    QCOMPARE(badHttpRequests, 0);
}

int runSdsClientTests(int argc, char** argv)
{
    TestSdsClient t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_sdsclient.moc"
