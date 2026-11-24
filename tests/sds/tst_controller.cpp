// SdsController with a fake clipboard and a fake network client: start sequence (host wins),
// echo suppression, permission gating, dropped files, hotkey and overlay messages.

#include "streaming/sds/sdscontroller.h"
#include "streaming/sds/fileclipboard.h"
#include "testutil.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Sds;

namespace {

class FakeClipboard : public IClipboardBackend
{
public:
    bool isAvailable() const override { return true; }
    qint64 changeCount() override { return count; }
    ReadResult read(qint64 textMax, qint64 imageMax) override
    {
        reads++;
        ReadResult r;
        if (content.text.size() > textMax) r.textTooLarge = true; else r.item.text = content.text;
        if (content.png.size() > imageMax) r.imageTooLarge = true; else r.item.png = content.png;
        return r;
    }
    qint64 write(const ClipboardItem& item) override
    {
        content = item;
        writes.append(item);
        return ++count;
    }
    bool hasFileSelection() override { return !fileBundle.isEmpty(); }
    QByteArray readFiles(QString*) override { return fileBundle; }
    qint64 writeFiles(const QByteArray& bundle, qint64 expected, QString*) override
    {
        if (expected != count) return -1;
        fileBundle = bundle;
        ++fileWrites;
        return ++count;
    }
    bool readFileEntries(QList<ClipboardEntryMeta>* entriesOut, QString*) override
    {
        *entriesOut = localEntries;
        return !localEntries.isEmpty();
    }
    QString createLazyStageDir(QString*) override { return lazyStagePath; }
    void retireStageDir(const QString& dir) override { retiredStageDirs.append(dir); }
    qint64 writeLazyOffer(const LazyClipboardOffer& offer,
                          std::shared_ptr<LazyTransferResult>,
                          LazyDemandCallback onDemand,
                          QString*) override
    {
        writtenOffers.append(offer);
        lastDemandCb = std::move(onDemand);
        return ++count;
    }
    bool completeLazyOffer(const QString& offerId,
                           bool ok,
                           const QStringList& stagedPaths,
                           const QByteArray& png,
                           const QString&,
                           qint64* newCc,
                           bool* needRepaste) override
    {
        Q_UNUSED(offerId);
        Q_UNUSED(png);
        if (ok && simulatedTimeoutRepaste) {
            if (needRepaste) *needRepaste = true;
            if (newCc) *newCc = ++count;
        }
        else {
            if (needRepaste) *needRepaste = false;
            if (newCc) *newCc = -1;
        }
        completedStagedPaths = stagedPaths;
        return ok;
    }
    void retireFiles() override { ++retirements; }

    // Simulates the user copying something in another app
    void userCopies(const QByteArray& text, const QByteArray& png = QByteArray())
    {
        fileBundle.clear();
        localEntries.clear();
        content.text = text;
        content.png = png;
        count++;
    }

    void userCopiesLazyFiles(const QList<ClipboardEntryMeta>& entries)
    {
        content = ClipboardItem();
        fileBundle = QByteArrayLiteral("lazy-marker");
        localEntries = entries;
        count++;
    }

    qint64 count = 100;
    ClipboardItem content;
    QList<ClipboardItem> writes;
    int reads = 0;
    QByteArray fileBundle;
    int fileWrites = 0;
    int retirements = 0;
    QList<ClipboardEntryMeta> localEntries;
    QString lazyStagePath = QStringLiteral("/tmp/fake-stage");
    QStringList retiredStageDirs;
    QList<LazyClipboardOffer> writtenOffers;
    LazyDemandCallback lastDemandCb;
    bool simulatedTimeoutRepaste = false;
    QStringList completedStagedPaths;
};

class FakeClient : public IClient
{
public:
    void start() override { started++; }
    void stop() override { stopped++; }
    void sendClipboard(const ClipboardItem& item) override { sent.append(item); }
    void sendFile(const QString& path) override { files.append(path); }
    void downloadOutboxNow() override { downloads++; }
    void exchangeFileClipboard(const QByteArray& bundle, qint64 generation) override
    { nativeBundles.append(bundle); nativeGenerations.append(generation); }
    void publishLazyClipboardOffer(const LazyClipboardOffer& offer) override { publishedOffers.append(offer); }
    void cancelLazyClipboardOffer(const QString& offerId) override { cancelledOfferIds.append(offerId); }
    void fulfillLazyClipboardPull(const LazyClipboardOffer& offer) override { fulfilledOffers.append(offer); }
    void fetchLazyClipboardOffer(const LazyClipboardOffer& offer,
                                 const QString& stageDir,
                                 std::shared_ptr<LazyTransferResult>) override
    {
        fetchedOffers.append({offer, stageDir});
    }
    void sendDroppedFiles(double nx, double ny, const QStringList& paths) override
    {
        droppedBatches.append({nx, ny, paths});
    }

    struct DropBatch { double nx; double ny; QStringList paths; };

    int started = 0;
    int stopped = 0;
    QList<ClipboardItem> sent;
    QStringList files;
    int downloads = 0;
    QList<QByteArray> nativeBundles;
    QList<qint64> nativeGenerations;
    QList<LazyClipboardOffer> publishedOffers;
    QStringList cancelledOfferIds;
    QList<LazyClipboardOffer> fulfilledOffers;
    QList<QPair<LazyClipboardOffer, QString>> fetchedOffers;
    QList<DropBatch> droppedBatches;
};

Capabilities caps(std::initializer_list<const char*> perms)
{
    Capabilities c;
    c.version = 1;
    c.streamActive = true;
    for (const char* p : perms) {
        c.permissions.insert(QString::fromLatin1(p));
    }
    return c;
}

ClientEvent ready(const Capabilities& c, bool hostProvided)
{
    ClientEvent ev;
    ev.type = ClientEvent::Ready;
    ev.caps = c;
    ev.hostProvidedClipboard = hostProvided;
    return ev;
}

ClientEvent hostClip(const QByteArray& text)
{
    ClientEvent ev;
    ev.type = ClientEvent::ClipboardReceived;
    ev.clipboard.text = text;
    ev.hostSeq = 7;
    return ev;
}

} // namespace

class TestController : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void readyPushesLocalClipboardWhenHostHasNone();
    void hostWinsAtStart();
    void pollPushesOnlyRealChanges();
    void hostContentIsNotEchoed();
    void permissionsGateClipboard();
    void oversizedLocalImageShowsMessage();
    void droppedFiles();
    void dropWhenUnavailable();
    void downloadHotkey();
    void messagesAutoHide();
    void wakeIsCoalesced();
    void stopStopsClient();
    void nativeFilesBaselineAndFreshCopy();
    void nativeFilesStalePublicationAndStop();
    void nativeFilePermissionConjunction();
    void lazyClipboardHostAndClientFlow();
    void multiFileCoordinateDropAndMapping();

private:
    void make(bool clipboard = true, bool files = true, const QString& reason = QString(), bool nativeFiles = false);
    void deliver(const ClientEvent& ev, quint64 now = 1000);

    FakeClipboard* m_Clip = nullptr;
    FakeClient* m_Client = nullptr;
    std::unique_ptr<SdsController> m_Ctl;
    QStringList m_Shown;
    int m_Hidden = 0;
    int m_Wakes = 0;
};

void TestController::init()
{
    m_Shown.clear();
    m_Hidden = 0;
    m_Wakes = 0;
}

void TestController::cleanup()
{
    m_Ctl.reset();
    m_Clip = nullptr;
    m_Client = nullptr;
}

void TestController::make(bool clipboard, bool files, const QString& reason, bool nativeFiles)
{
    SdsController::Options o;
    o.clipboard = clipboard;
    o.files = files;
    o.fileClipboard = nativeFiles;
    o.unavailableReason = reason;

    SdsController::Hooks h;
    h.wake = [this]() { m_Wakes++; };
    h.showStatus = [this](const QString& t) { m_Shown.append(t); };
    h.hideStatus = [this]() { m_Hidden++; };

    auto clip = std::make_unique<FakeClipboard>();
    m_Clip = clip.get();
    m_Ctl = std::make_unique<SdsController>(o, std::move(clip), h);
    auto client = std::make_unique<FakeClient>();
    m_Client = client.get();
    m_Ctl->attachClient(std::move(client));
}

void TestController::deliver(const ClientEvent& ev, quint64 now)
{
    m_Ctl->onSdsClientEvent(ev);
    m_Ctl->processPendingEvents(now);
}

void TestController::nativeFilesBaselineAndFreshCopy()
{
    make(true, true, {}, true);
    m_Clip->fileBundle = encodeFileClipboard({{QStringLiteral("preexisting.txt"), "old"}});
    m_Ctl->start(0);
    deliver(ready(caps({kPermClipboardRead, kPermClipboardWrite, kPermFileUpload, kPermFileDownload}), false));
    m_Ctl->tick(1000);
    QCOMPARE(m_Client->nativeBundles.size(), 1);
    QVERIFY(m_Client->nativeBundles[0].isEmpty());
    QVERIFY(m_Client->sent.isEmpty());
    ClientEvent baseline;
    baseline.type = ClientEvent::FileClipboardObserved;
    baseline.localGeneration = m_Clip->count;
    deliver(baseline);
    m_Clip->fileBundle = encodeFileClipboard({{QStringLiteral("fresh.txt"), "new"}});
    ++m_Clip->count;
    m_Ctl->tick(1100); // fresh change bypasses poll throttle
    QCOMPARE(m_Client->nativeBundles.size(), 2);
    QCOMPARE(m_Client->nativeBundles.last(), m_Clip->fileBundle);
    const auto count = m_Client->nativeBundles.size();
    m_Ctl->tick(1200);
    QCOMPARE(m_Client->nativeBundles.size(), count);
    m_Clip->userCopies("ordinary text");
    m_Ctl->tick(1300); // empty exchange cancels a pending native bundle immediately
    QVERIFY(m_Client->nativeBundles.last().isEmpty());
    QCOMPARE(m_Client->nativeGenerations.last(), m_Clip->count);
}

void TestController::nativeFilesStalePublicationAndStop()
{
    make(true, true, {}, true);
    m_Ctl->start(0);
    deliver(ready(caps({kPermClipboardRead, kPermFileDownload}), true));
    ClientEvent event;
    event.type = ClientEvent::FileClipboardObserved;
    event.fileBundle = encodeFileClipboard({{QStringLiteral("remote.txt"), "bytes"}});
    event.localGeneration = m_Clip->count;
    m_Clip->userCopies("newer user text");
    deliver(event);
    QCOMPARE(m_Clip->fileWrites, 0);
    event.localGeneration = m_Clip->count;
    deliver(event);
    QCOMPARE(m_Clip->fileWrites, 1);
    const auto afterWrite = m_Clip->count;
    m_Ctl->tick(1000);
    for (const auto& bundle : m_Client->nativeBundles) QVERIFY(bundle.isEmpty());
    event.localGeneration = afterWrite;
    m_Ctl->onSdsClientEvent(event);
    m_Ctl->stop();
    m_Ctl->processPendingEvents(2000);
    m_Ctl->tick(2100);
    QCOMPARE(m_Clip->fileWrites, 1);
    QVERIFY(m_Ctl->isStopped());
    QVERIFY(!m_Ctl->isReady());
}

void TestController::nativeFilePermissionConjunction()
{
    const auto bundle = encodeFileClipboard({{QStringLiteral("remote.txt"), "bytes"}});
    for (const auto& permissions : {caps({kPermClipboardRead}), caps({kPermFileDownload}),
                                  caps({kPermClipboardWrite}), caps({kPermFileUpload})}) {
        make(true, true, {}, true);
        m_Ctl->start(0);
        deliver(ready(permissions, true));
        ClientEvent event;
        event.type = ClientEvent::FileClipboardObserved;
        event.fileBundle = bundle;
        event.localGeneration = m_Clip->count;
        deliver(event);
        m_Clip->fileBundle = bundle;
        ++m_Clip->count;
        m_Ctl->tick(1000);
        QCOMPARE(m_Clip->fileWrites, 0);
        QVERIFY(m_Client->nativeBundles.isEmpty());
    }
}

void TestController::readyPushesLocalClipboardWhenHostHasNone()
{
    make();
    m_Clip->userCopies("mac text");
    m_Ctl->start(1000);
    QCOMPARE(m_Client->started, 1);

    // Nothing is pushed before the capabilities are known
    m_Ctl->tick(1300);
    QVERIFY(m_Client->sent.isEmpty());

    deliver(ready(caps({ kPermClipboardRead, kPermClipboardWrite }), false));
    QCOMPARE(m_Client->sent.size(), 1);
    QCOMPARE(m_Client->sent[0].text, QByteArray("mac text"));

    // Not pushed again by the poll
    m_Ctl->tick(1600);
    QCOMPARE(m_Client->sent.size(), 1);
}

void TestController::hostWinsAtStart()
{
    make();
    m_Clip->userCopies("mac text");
    m_Ctl->start(1000);

    deliver(ready(caps({ kPermClipboardRead, kPermClipboardWrite }), true));
    QVERIFY(m_Client->sent.isEmpty()); // host announced content: do not push ours

    deliver(hostClip("host text"));
    QCOMPARE(m_Clip->content.text, QByteArray("host text"));
    m_Ctl->tick(1300);
    QVERIFY(m_Client->sent.isEmpty());
}

void TestController::pollPushesOnlyRealChanges()
{
    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermClipboardWrite }), false));
    QVERIFY(m_Client->sent.isEmpty()); // empty clipboard: nothing to push

    m_Clip->userCopies("a");
    m_Ctl->tick(1300);
    QCOMPARE(m_Client->sent.size(), 1);

    m_Ctl->tick(1600);
    QCOMPARE(m_Client->sent.size(), 1);

    m_Clip->userCopies("a"); // same content copied again
    m_Ctl->tick(1900);
    QCOMPARE(m_Client->sent.size(), 1);

    QByteArray png = SdsTest::makePng(3, 3);
    m_Clip->userCopies("b", png);
    m_Ctl->tick(2200);
    QCOMPARE(m_Client->sent.size(), 2);
    QCOMPARE(m_Client->sent[1].text, QByteArray("b"));
    QCOMPARE(m_Client->sent[1].png, png);
}

void TestController::hostContentIsNotEchoed()
{
    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermClipboardRead, kPermClipboardWrite }), false));

    deliver(hostClip("from host"));
    QCOMPARE(m_Clip->writes.size(), 1);
    m_Ctl->tick(1300);
    m_Ctl->tick(1600);
    QVERIFY(m_Client->sent.isEmpty()); // our own write is not pushed back

    // The same host content again does not rewrite the clipboard
    deliver(hostClip("from host"));
    QCOMPARE(m_Clip->writes.size(), 1);

    // Invalid host content (validated again at the point of use) is not written
    deliver(hostClip(QByteArray("bad\0text", 8)));
    QCOMPARE(m_Clip->writes.size(), 1);

    m_Clip->userCopies("user again");
    m_Ctl->tick(1900);
    QCOMPARE(m_Client->sent.size(), 1);
}

void TestController::permissionsGateClipboard()
{
    // Read only: local changes are not pushed
    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermClipboardRead }), false));
    m_Clip->userCopies("secret");
    m_Ctl->tick(1300);
    QVERIFY(m_Client->sent.isEmpty());
    QCOMPARE(m_Clip->reads, 0);
    deliver(hostClip("ok"));
    QCOMPARE(m_Clip->content.text, QByteArray("ok"));

    // Write only: host content is ignored
    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermClipboardWrite }), false));
    deliver(hostClip("ignored"));
    QVERIFY(m_Clip->writes.isEmpty());

    // Clipboard disabled by the user: nothing either way
    make(false, true);
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermClipboardRead, kPermClipboardWrite }), false));
    m_Clip->userCopies("x");
    m_Ctl->tick(1300);
    deliver(hostClip("y"));
    QVERIFY(m_Client->sent.isEmpty());
    QVERIFY(m_Clip->writes.isEmpty());
}

void TestController::oversizedLocalImageShowsMessage()
{
    make();
    m_Ctl->start(1000);
    Capabilities c = caps({ kPermClipboardWrite });
    c.limits.clipboardImageMax = 10;
    deliver(ready(c, false));

    m_Clip->userCopies(QByteArray(), SdsTest::makePng(8, 8));
    m_Ctl->tick(1300);
    QVERIFY(m_Client->sent.isEmpty());
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Clipboard image too large"));
}

void TestController::droppedFiles()
{
    QTemporaryDir tmp;
    QString file = tmp.filePath(QStringLiteral("report.pdf"));
    QFile f(file);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("pdf");
    f.close();

    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermFileUpload }), false));

    m_Ctl->fileDropped(tmp.path(), 1100);
    QVERIFY(m_Client->files.isEmpty());
    QVERIFY(m_Ctl->currentStatus().contains(QStringLiteral("Folders cannot be sent")));

    m_Ctl->fileDropped(tmp.filePath(QStringLiteral("missing.txt")), 1200);
    QVERIFY(m_Client->files.isEmpty());

    m_Ctl->fileDropped(file, 1300);
    QCOMPARE(m_Client->files, QStringList() << file);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Sending report.pdf"));

    // Progress and completion messages
    ClientEvent p;
    p.type = ClientEvent::TransferProgress;
    p.upload = true;
    p.name = QStringLiteral("report.pdf");
    p.percent = 45;
    deliver(p, 1400);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Sending report.pdf 45%"));

    ClientEvent done;
    done.type = ClientEvent::TransferFinished;
    done.upload = true;
    done.ok = true;
    done.name = QStringLiteral("report (1).pdf");
    deliver(done, 1500);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Sent report (1).pdf"));

    // No upload permission
    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermFileDownload }), false));
    m_Ctl->fileDropped(file, 1100);
    QVERIFY(m_Client->files.isEmpty());
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("The host does not allow sending files"));
}

void TestController::dropWhenUnavailable()
{
    make(true, false, QStringLiteral("This host does not support file transfer"));
    m_Ctl->start(1000);
    m_Ctl->fileDropped(QStringLiteral("/etc/hosts"), 1100);
    QVERIFY(m_Client->files.isEmpty());
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("This host does not support file transfer"));

    // Client stopped (feature switched off on the host)
    make();
    m_Ctl->start(1000);
    ClientEvent stopped;
    stopped.type = ClientEvent::Stopped;
    stopped.message = QStringLiteral("Clipboard and file transfer are disabled on the host");
    deliver(stopped);
    QVERIFY(m_Ctl->isStopped());
    m_Ctl->fileDropped(QStringLiteral("/etc/hosts"), 1200);
    QVERIFY(m_Client->files.isEmpty());
    QCOMPARE(m_Ctl->currentStatus(), stopped.message);
}

void TestController::downloadHotkey()
{
    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermFileDownload }), false));
    m_Ctl->downloadOutboxNow(1100);
    QCOMPARE(m_Client->downloads, 1);

    make();
    m_Ctl->start(1000);
    deliver(ready(caps({ kPermFileUpload }), false));
    m_Ctl->downloadOutboxNow(1100);
    QCOMPARE(m_Client->downloads, 0);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("The host does not allow downloading files"));
}

void TestController::messagesAutoHide()
{
    make();
    m_Ctl->start(1000);
    ClientEvent n;
    n.type = ClientEvent::Notice;
    n.message = QStringLiteral("Clipboard too large for the host");
    deliver(n, 5000);
    QCOMPARE(m_Shown.last(), n.message);

    m_Ctl->tick(5000 + SdsController::kMessageMs - 1);
    QCOMPARE(m_Ctl->currentStatus(), n.message);
    QCOMPARE(m_Hidden, 0);

    m_Ctl->tick(5000 + SdsController::kMessageMs);
    QVERIFY(m_Ctl->currentStatus().isEmpty());
    QCOMPARE(m_Hidden, 1);
}

void TestController::wakeIsCoalesced()
{
    make();
    m_Ctl->start(1000);
    ClientEvent n;
    n.type = ClientEvent::Notice;
    n.message = QStringLiteral("one");
    m_Ctl->onSdsClientEvent(n);
    n.message = QStringLiteral("two");
    m_Ctl->onSdsClientEvent(n);
    QCOMPARE(m_Wakes, 1);

    m_Ctl->processPendingEvents(1100);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("two"));

    m_Ctl->onSdsClientEvent(n);
    QCOMPARE(m_Wakes, 2);
}

void TestController::stopStopsClient()
{
    make();
    m_Ctl->start(1000);
    m_Ctl->stop();
    QCOMPARE(m_Client->stopped, 1);
}

void TestController::lazyClipboardHostAndClientFlow()
{
    make(true, true, QString(), true);
    m_Ctl->start(1000);
    Capabilities c = caps({ kPermClipboardRead, kPermClipboardWrite, kPermFileUpload, kPermFileDownload });
    c.features.insert(QStringLiteral("lazy_clipboard"));
    deliver(ready(c, false));

    // Host announces lazy offer -> client registers lazy pasteboard with 0 fetches until demand.
    ClientEvent offerEv;
    offerEv.type = ClientEvent::LazyClipboardOfferReceived;
    offerEv.lazyOffer.offerId = QStringLiteral("host_1");
    offerEv.lazyOffer.kind = QStringLiteral("files");
    offerEv.lazyOffer.fromHost = true;
    offerEv.lazyOffer.entries.append({QStringLiteral("e0"), QStringLiteral("big.bin"), 4096, QString()});
    deliver(offerEv, 1100);
    QCOMPARE(m_Clip->writtenOffers.size(), 1);
    QVERIFY(m_Client->fetchedOffers.isEmpty());
    QVERIFY(m_Clip->lastDemandCb);

    // Consumer paste triggers demand fetch once; slow completion shows repaste banner.
    m_Clip->simulatedTimeoutRepaste = true;
    m_Clip->lastDemandCb(QStringLiteral("host_1"));
    QCOMPARE(m_Client->fetchedOffers.size(), 1);
    QCOMPARE(m_Client->fetchedOffers[0].first.offerId, QStringLiteral("host_1"));
    QCOMPARE(m_Client->fetchedOffers[0].second, m_Clip->lazyStagePath);

    ClientEvent doneEv;
    doneEv.type = ClientEvent::LazyClipboardFinished;
    doneEv.offerId = QStringLiteral("host_1");
    doneEv.lazyOffer = offerEv.lazyOffer;
    doneEv.ok = true;
    doneEv.stagedPaths = QStringList{m_Clip->lazyStagePath + QStringLiteral("/big.bin")};
    deliver(doneEv, 1200);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Files ready — press Cmd+V"));

    // Local file copy publishes metadata only (zero pull until host demands it).
    m_Clip->userCopiesLazyFiles({{QStringLiteral("e0"), QStringLiteral("notes.txt"), 128, QStringLiteral("/tmp/notes.txt")}});
    m_Ctl->tick(1500);
    QCOMPARE(m_Client->publishedOffers.size(), 1);
    const QString localId = m_Client->publishedOffers[0].offerId;
    QVERIFY(m_Client->fulfilledOffers.isEmpty());

    ClientEvent pullEv;
    pullEv.type = ClientEvent::LazyClipboardPullRequested;
    pullEv.offerId = localId;
    pullEv.pullTargets = QStringList{QStringLiteral("text/uri-list")};
    deliver(pullEv, 1600);
    QCOMPARE(m_Client->fulfilledOffers.size(), 1);
    QCOMPARE(m_Client->fulfilledOffers[0].offerId, localId);
}

void TestController::multiFileCoordinateDropAndMapping()
{
    double nx = 0.0, ny = 0.0;
    // Letterboxed: 1920x1080 stream in 1280x1024 window -> dst = {0, 152, 1280, 720}
    mapPointToNormalizedVideoRect(1280, 1024, 1920, 1080, 640, 512, &nx, &ny);
    QVERIFY(qAbs(nx - 0.5) < 0.01);
    QVERIFY(qAbs(ny - 0.5) < 0.01);
    // Pointer in top letterbox bar clamps ny -> 0.0
    mapPointToNormalizedVideoRect(1280, 1024, 1920, 1080, 320, 10, &nx, &ny);
    QVERIFY(qAbs(nx - 0.25) < 0.01);
    QCOMPARE(ny, 0.0);
    // Pillarboxed: 1920x1080 stream in 2560x1080 window -> dst = {320, 0, 1920, 1080}
    mapPointToNormalizedVideoRect(2560, 1080, 1920, 1080, 320 + 960, 270, &nx, &ny);
    QVERIFY(qAbs(nx - 0.5) < 0.01);
    QVERIFY(qAbs(ny - 0.25) < 0.01);

    QTemporaryDir tmp;
    const QString f1 = tmp.filePath(QStringLiteral("a.txt"));
    const QString f2 = tmp.filePath(QStringLiteral("b.txt"));
    for (const QString& p : {f1, f2}) {
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("drop");
    }

    make();
    m_Ctl->start(1000);
    Capabilities c = caps({ kPermFileUpload });
    c.features.insert(QStringLiteral("xdnd_drop"));
    deliver(ready(c, false));

    m_Ctl->filesDropped(0.25, 0.75, {f1, f2}, 1300);
    QCOMPARE(m_Client->droppedBatches.size(), 1);
    QVERIFY(qAbs(m_Client->droppedBatches[0].nx - 0.25) < 1e-6);
    QVERIFY(qAbs(m_Client->droppedBatches[0].ny - 0.75) < 1e-6);
    QCOMPARE(m_Client->droppedBatches[0].paths, (QStringList{f1, f2}));
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Sending 2 files"));

    ClientEvent done;
    done.type = ClientEvent::TransferFinished;
    done.upload = true;
    done.ok = true;
    done.name = QStringLiteral("2 files");
    done.message = QStringLiteral("Dropped 2 files");
    deliver(done, 1400);
    QCOMPARE(m_Ctl->currentStatus(), QStringLiteral("Dropped 2 files"));
}

int runControllerTests(int argc, char** argv)
{
    TestController t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_controller.moc"
