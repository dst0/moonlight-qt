#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/sdslaunchparams.h"
#ifdef Q_OS_DARWIN
#include "backend/keychain_mac.h"
#endif
#include "testcerts.h"

#include <Limelight.h>
#include <QMutex>
#include <QSettings>
#include <QSslServer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

// The unrelated launch path is linked but never exercised by these HTTP tests.
extern "C" const char* LiGetLaunchUrlQueryParameters() { return ""; }

namespace {
QMutex logLock;
QStringList observedLogs;

void collectHttpLogs(QtMsgType, const QMessageLogContext&, const QString& message)
{
    const QMutexLocker locker(&logLock);
    observedLogs.append(message);
}

class ScopedHttpLogCapture
{
public:
    ScopedHttpLogCapture()
    {
        const QMutexLocker locker(&logLock);
        observedLogs.clear();
        m_Previous = qInstallMessageHandler(collectHttpLogs);
    }
    ~ScopedHttpLogCapture() { qInstallMessageHandler(m_Previous); }
    QString text() const
    {
        const QMutexLocker locker(&logLock);
        return observedLogs.join(QLatin1Char('\n'));
    }

private:
    QtMessageHandler m_Previous;
};
}

class TestNvHttp : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_Profile.isValid());
        QCoreApplication::setOrganizationName("SecureDesktopStreamTests");
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_Profile.path());
#ifdef Q_OS_DARWIN
        QSettings settings;
        m_Service = Keychain::identityServiceForProfile(settings.fileName());
#endif
    }

    void cleanupTestCase()
    {
#ifdef Q_OS_DARWIN
        QString error;
        QVERIFY(Keychain::remove(m_Service, Keychain::ClientKeyAccount, &error) != Keychain::Status::Error);
#endif
    }

    void localServerInfoWorks()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0;
        respond(server, requests, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
                "<root status_code=\"200\"><HttpsPort>47984</HttpsPort></root>");
        NvHTTP http(NvAddress("127.0.0.1", server.serverPort()), 0, QSslCertificate(), false);
        QVERIFY(http.getServerInfo(NvHTTP::NVLL_ERROR).contains("HttpsPort"));
        QCOMPARE(requests, 1);
    }

    void verbosePairingRequestOmitsSensitiveQueryFromLog()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0;
        respond(server, requests, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
                "<root status_code=\"200\"><paired>1</paired></root>");
        NvHTTP http(NvAddress("127.0.0.1", server.serverPort()), 0, QSslCertificate(), true);
        const QString marker = QStringLiteral("sds-test-only-challenge");
        QString response;
        QString logs;
        {
            const ScopedHttpLogCapture capture;
            response = http.openConnectionToString(http.m_BaseUrlHttp, QStringLiteral("pair"),
                                                   QStringLiteral("clientchallenge=") + marker, 2000,
                                                   NvHTTP::NVLL_VERBOSE);
            logs = capture.text();
        }
        QCOMPARE(requests, 1);
        QVERIFY(response.contains(QStringLiteral("<paired>1</paired>")));
        QVERIFY(logs.contains(QStringLiteral("Executing request:")));
        QVERIFY(!logs.contains(marker));
        QVERIFY(!logs.contains(QStringLiteral("uniqueid=")));
    }

    void redirectsAreRejectedWithoutContactingTheirDestination()
    {
        QTcpServer origin, destination;
        QVERIFY(origin.listen(QHostAddress::LocalHost));
        QVERIFY(destination.listen(QHostAddress::LocalHost));
        int originRequests = 0, destinationRequests = 0;
        respond(origin, originRequests, "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: http://127.0.0.1:"
                + QByteArray::number(destination.serverPort()) + "/redirect-target\r\n\r\n");
        respond(destination, destinationRequests, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
                "<root status_code=\"200\"><HttpsPort>47984</HttpsPort></root>");
        NvHTTP http(NvAddress("127.0.0.1", origin.serverPort()), 0, QSslCertificate(), false);
        bool refused = false;
        try { http.getServerInfo(NvHTTP::NVLL_ERROR); }
        catch (const GfeHttpResponseException&) { refused = true; }
        QCOMPARE(originRequests, 1);
        QCOMPARE(destinationRequests, 0);
        QVERIFY(refused);
    }

    void pinMismatchDoesNotFallbackToHttp()
    {
        const TestCert pinned = TestCerts::makeSelfSigned("Pinned Host");
        const TestCert impostor = TestCerts::makeSelfSigned("Impostor Host");

        QTcpServer httpServer;
        QVERIFY(httpServer.listen(QHostAddress::LocalHost));
        int httpRequests = 0;
        respond(httpServer, httpRequests, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
                "<root status_code=\"200\"><HttpsPort>47984</HttpsPort></root>");

        QSslServer httpsServer;
        QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
        sslCfg.setLocalCertificate(impostor.cert());
        sslCfg.setPrivateKey(impostor.key());
        sslCfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        httpsServer.setSslConfiguration(sslCfg);
        QVERIFY(httpsServer.listen(QHostAddress::LocalHost));
        int httpsRequests = 0;
        connect(&httpsServer, &QSslServer::pendingConnectionAvailable, &httpsServer, [&]() {
            while (QTcpSocket* socket = httpsServer.nextPendingConnection()) {
                ++httpsRequests;
                socket->disconnectFromHost();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });

        NvHTTP http(NvAddress("127.0.0.1", httpServer.serverPort()),
                    httpsServer.serverPort(), pinned.cert(), false);
        int statusCode = 0;
        try {
            http.getServerInfo(NvHTTP::NVLL_ERROR);
        } catch (const GfeHttpResponseException& e) {
            statusCode = e.getStatusCode();
        }
        QTest::qWait(200);
        QCOMPARE(statusCode, 495);
        QCOMPARE(httpRequests, 0);
        QCOMPARE(httpsRequests, 0);
    }

    void rikeyErrorRedaction()
    {
        const TestCert pinned = TestCerts::makeSelfSigned("Pinned Launch Host");
        QSslServer httpsServer;
        QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
        sslCfg.setLocalCertificate(pinned.cert());
        sslCfg.setPrivateKey(pinned.key());
        sslCfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        httpsServer.setSslConfiguration(sslCfg);
        QVERIFY(httpsServer.listen(QHostAddress::LocalHost));
        connect(&httpsServer, &QSslServer::pendingConnectionAvailable, &httpsServer, [&]() {
            while (QTcpSocket* socket = httpsServer.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                    if (socket->property("responded").toBool()) return;
                    const auto req = socket->property("req").toByteArray() + socket->readAll();
                    if (!req.contains("\r\n\r\n")) {
                        socket->setProperty("req", req);
                        return;
                    }
                    socket->setProperty("responded", true);
                    socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 4\r\nConnection: close\r\n\r\nfail");
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });

        NvHTTP http(NvAddress("127.0.0.1", 47989), httpsServer.serverPort(), pinned.cert(), true);
        STREAM_CONFIGURATION cfg{};
        cfg.width = 1920;
        cfg.height = 1080;
        cfg.fps = 60;
        for (size_t i = 0; i < sizeof(cfg.remoteInputAesKey); ++i) {
            cfg.remoteInputAesKey[i] = static_cast<char>(0xAB);
        }
        const QString secretHex = QByteArray(cfg.remoteInputAesKey, sizeof(cfg.remoteInputAesKey)).toHex();

        QString rtspUrl;
        bool caught = false;
        QString errMsg;
        QString errWhat;
        try {
            http.startApp(QStringLiteral("launch"), false, 1, &cfg, false, false, 0, false, rtspUrl);
        } catch (const QtNetworkReplyException& e) {
            caught = true;
            errMsg = e.toQString();
            errWhat = QString::fromUtf8(e.what());
        }
        QVERIFY(caught);
        QVERIFY(!errMsg.contains(secretHex, Qt::CaseInsensitive));
        QVERIFY(!errMsg.contains(QStringLiteral("rikey"), Qt::CaseInsensitive));
        QVERIFY(!errMsg.contains(QStringLiteral("uniqueid"), Qt::CaseInsensitive));
        QVERIFY(!errMsg.contains(QLatin1Char('?')));
        QVERIFY(!errWhat.contains(secretHex, Qt::CaseInsensitive));
        QVERIFY(!errWhat.contains(QStringLiteral("rikey"), Qt::CaseInsensitive));
        QVERIFY(!errWhat.contains(QLatin1Char('?')));
    }

    void parsesSdsEncodersAdaptersAndBuildsLaunchQuery()
    {
        const QString serverInfo = QStringLiteral(
            "<root status_code=\"200\">"
            "<hostname>sds-host-linux</hostname>"
            "<appversion>7.1.431.-1</appversion>"
            "<uniqueid>0123456789ABCDEF</uniqueid>"
            "<HttpsPort>47984</HttpsPort>"
            "<ExternalPort>47989</ExternalPort>"
            "<ServerCodecModeSupport>65793</ServerCodecModeSupport>"
            "<MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>"
            "<sdsVersion>1</sdsVersion>"
            "<sdsFeatures>clipboard,files,file_clipboard,lazy_clipboard,xdnd_drop,host_encoder,host_capture_adapter,host_resolution</sdsFeatures>"
            "<sdsEncoders>vaapi,vulkan,software</sdsEncoders>"
            "<sdsSoftwareCodecModeSupport>786689</sdsSoftwareCodecModeSupport>"
            "<sdsAdapters>"
            "  <Adapter><ID>/dev/dri/by-path/pci-0000:01:00.0-render</ID><Name>Example Discrete GPU A (01:00.0)</Name><PciSlot>0000:01:00.0</PciSlot><Default>1</Default><CaptureDefault>0</CaptureDefault></Adapter>"
            "  <Adapter><ID>/dev/dri/by-path/pci-0000:02:00.0-render</ID><Name>Example Discrete GPU B (02:00.0)</Name><PciSlot>0000:02:00.0</PciSlot><Default>0</Default><CaptureDefault>1</CaptureDefault></Adapter>"
            "</sdsAdapters>"
            "</root>");

        NvHTTP http(NvAddress(QStringLiteral("127.0.0.1"), 47989), 47984, QSslCertificate(), true);
        NvComputer pc(http, serverInfo);
        QCOMPARE(pc.sdsVersion, 1);
        QVERIFY(pc.hasSdsFeature(QStringLiteral("host_encoder")));
        QVERIFY(pc.hasSdsFeature(QStringLiteral("host_capture_adapter")));
        QVERIFY(pc.hasSdsFeature(QStringLiteral("host_resolution")));
        QVERIFY(pc.hasSdsFeature(QStringLiteral("lazy_clipboard")));
        QVERIFY(pc.hasSdsFeature(QStringLiteral("xdnd_drop")));
        QCOMPARE(pc.sdsEncoders, (QStringList{QStringLiteral("vaapi"), QStringLiteral("vulkan"), QStringLiteral("software")}));
        QCOMPARE(pc.sdsSoftwareCodecModeSupport, 786689);
        QCOMPARE(pc.sdsAdapters.size(), 2);
        QCOMPARE(pc.sdsAdapters[0].id, QStringLiteral("/dev/dri/by-path/pci-0000:01:00.0-render"));
        QCOMPARE(pc.sdsAdapters[0].pciSlot, QStringLiteral("0000:01:00.0"));
        QVERIFY(pc.sdsAdapters[0].isDefault);
        QVERIFY(!pc.sdsAdapters[0].isCaptureDefault);
        QCOMPARE(pc.sdsAdapters[1].name, QStringLiteral("Example Discrete GPU B (02:00.0)"));
        QCOMPARE(pc.sdsAdapters[1].pciSlot, QStringLiteral("0000:02:00.0"));
        QVERIFY(!pc.sdsAdapters[1].isDefault);
        QVERIFY(pc.sdsAdapters[1].isCaptureDefault);

        // Effective codec modes: software encoder uses sdsSoftwareCodecModeSupport (enabling H264 4:4:4 even if idle GPU had none)
        const int gpuOnlyModes = SCM_H264 | SCM_HEVC;
        QCOMPARE(SdsLaunch::effectiveServerCodecModeSupport(
                     false, true, gpuOnlyModes, SCM_H264 | SCM_H264_HIGH8_444,
                     StreamingPreferences::HEC_SOFTWARE),
                 SCM_H264 | SCM_H264_HIGH8_444);
        QCOMPARE(SdsLaunch::effectiveServerCodecModeSupport(
                     false, true, gpuOnlyModes, 0,
                     StreamingPreferences::HEC_SOFTWARE),
                 SCM_H264 | SCM_H264_HIGH8_444);
        QCOMPARE(SdsLaunch::effectiveServerCodecModeSupport(
                     false, true, gpuOnlyModes, SCM_H264 | SCM_H264_HIGH8_444,
                     StreamingPreferences::HEC_HARDWARE_VAAPI),
                 gpuOnlyModes);

        // Software HEVC support reflects sdsSoftwareCodecModeSupport SCM_MASK_HEVC rather than hardware maxLumaPixelsHEVC
        QVERIFY(!SdsLaunch::isHostHevcSupported(false, true, 1869449984, SCM_H264 | SCM_H264_HIGH8_444,
                                                StreamingPreferences::HEC_SOFTWARE));
        QVERIFY(SdsLaunch::isHostHevcSupported(false, true, 0, SCM_H264 | SCM_H264_HIGH8_444 | SCM_HEVC | SCM_HEVC_REXT8_444,
                                               StreamingPreferences::HEC_SOFTWARE));
        QVERIFY(SdsLaunch::shouldPreferSoftwareH264Yuv444(
                    false, true, StreamingPreferences::HEC_SOFTWARE,
                    StreamingPreferences::VCC_AUTO, true, SCM_H264 | SCM_H264_HIGH8_444));
        QVERIFY(!SdsLaunch::shouldPreferSoftwareH264Yuv444(
                    false, true, StreamingPreferences::HEC_SOFTWARE,
                    StreamingPreferences::VCC_FORCE_HEVC, true, SCM_H264 | SCM_H264_HIGH8_444));

        // Query parameters: default match/revert unchecked, valid adapter included, stale adapter dropped, mismatch guard on revert
        const QString defaultQuery = SdsLaunch::buildLaunchExtraQueryParams(
                    false, true, true, StreamingPreferences::HEC_AUTO,
                    pc.sdsAdapters, QString(), false, false, false);
        QCOMPARE(defaultQuery, QStringLiteral("&sdsEncoder=auto&sdsHevc=0&sdsMatchRes=0&sdsRevertRes=0"));

        // Stale revert=true with match=false must NEVER send sdsRevertRes=1
        const QString edgeRevertOff = SdsLaunch::buildLaunchExtraQueryParams(
                    false, true, true, StreamingPreferences::HEC_HARDWARE_VULKAN,
                    pc.sdsAdapters, QStringLiteral("/dev/dri/by-path/pci-9999:00:00.0-render"), true, false, true);
        QCOMPARE(edgeRevertOff, QStringLiteral("&sdsEncoder=vulkan&sdsHevc=1&sdsMatchRes=0&sdsRevertRes=0"));

        // Valid adapter + independent capture adapter + match without revert
        const QString matchKeep = SdsLaunch::buildLaunchExtraQueryParams(
                    false, true, true, StreamingPreferences::HEC_SOFTWARE,
                    pc.sdsAdapters, pc.sdsAdapters[1].id, true, true, false,
                    true, pc.sdsAdapters[0].id);
        QVERIFY(matchKeep.contains(QStringLiteral("&sdsEncoder=software")));
        QVERIFY(matchKeep.contains(QStringLiteral("&sdsAdapter=%2Fdev%2Fdri%2Fby-path%2Fpci-0000%3A02%3A00.0-render")));
        QVERIFY(matchKeep.contains(QStringLiteral("&sdsCaptureAdapter=%2Fdev%2Fdri%2Fby-path%2Fpci-0000%3A01%3A00.0-render")));
        QVERIFY(matchKeep.contains(QStringLiteral("&sdsHevc=1&sdsMatchRes=1&sdsRevertRes=0")));

        // Unknown capture adapter ID must be dropped fail-closed
        const QString unknownCaptureDropped = SdsLaunch::buildLaunchExtraQueryParams(
                    false, true, true, StreamingPreferences::HEC_AUTO,
                    pc.sdsAdapters, QString(), false, false, false,
                    true, QStringLiteral("/dev/dri/by-path/pci-9999:00:00.0-render"));
        QVERIFY(!unknownCaptureDropped.contains(QStringLiteral("sdsCaptureAdapter")));

        // Match + explicit revert checked
        const QString matchAndRevert = SdsLaunch::buildLaunchExtraQueryParams(
                    false, true, true, StreamingPreferences::HEC_HARDWARE_VAAPI,
                    pc.sdsAdapters, QString(), false, true, true);
        QCOMPARE(matchAndRevert, QStringLiteral("&sdsEncoder=vaapi&sdsHevc=0&sdsMatchRes=1&sdsRevertRes=1"));

        // Nvidia GFE host receives zero sds query params
        QCOMPARE(SdsLaunch::buildLaunchExtraQueryParams(
                     true, true, true, StreamingPreferences::HEC_SOFTWARE,
                     pc.sdsAdapters, pc.sdsAdapters[0].id, true, true, true,
                     true, pc.sdsAdapters[1].id),
                 QString());
    }

    void startAppSdsChromaModeSignalsPacked444WithoutHdrMode()
    {
        const TestCert pinned = TestCerts::makeSelfSigned("Pinned Chroma Host");
        QSslServer httpsServer;
        QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
        sslCfg.setLocalCertificate(pinned.cert());
        sslCfg.setPrivateKey(pinned.key());
        sslCfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        httpsServer.setSslConfiguration(sslCfg);
        QVERIFY(httpsServer.listen(QHostAddress::LocalHost));

        QStringList capturedRequests;
        connect(&httpsServer, &QSslServer::pendingConnectionAvailable, &httpsServer, [&]() {
            while (QTcpSocket* socket = httpsServer.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket, &capturedRequests]() {
                    if (socket->property("responded").toBool()) return;
                    const auto req = socket->property("req").toByteArray() + socket->readAll();
                    if (!req.contains("\r\n\r\n")) {
                        socket->setProperty("req", req);
                        return;
                    }
                    socket->setProperty("responded", true);
                    capturedRequests.append(QString::fromUtf8(req.split('\n').first()));
                    const QByteArray body = "<root status_code=\"200\"><sessionUrl0>rtsp://127.0.0.1:48010</sessionUrl0></root>";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });

        NvHTTP http(NvAddress("127.0.0.1", 47989), httpsServer.serverPort(), pinned.cert(), true);
        STREAM_CONFIGURATION cfg{};
        cfg.width = 1920;
        cfg.height = 1080;
        cfg.fps = 60;
        cfg.supportedVideoFormats = VIDEO_FORMAT_H265_MAIN10;

        QString rtspUrl;
        http.startApp(QStringLiteral("launch"), false, 1, &cfg, false, false, 0, false,
                      rtspUrl, QStringLiteral("ycocgr_p010_a1"));
        QCOMPARE(rtspUrl, QStringLiteral("rtsp://127.0.0.1:48010"));
        QCOMPARE(capturedRequests.size(), 1);
        QVERIFY(capturedRequests[0].contains(QStringLiteral("mode=1920x1080x60")));
        QVERIFY(capturedRequests[0].contains(QStringLiteral("sdsChromaMode=ycocgr_p010_a1")));
        QVERIFY(!capturedRequests[0].contains(QStringLiteral("hdrMode=1")));

        // Standard 10-bit HDR without sdsChromaMode still sends hdrMode=1
        http.startApp(QStringLiteral("launch"), false, 1, &cfg, false, false, 0, false, rtspUrl);
        QCOMPARE(capturedRequests.size(), 2);
        QVERIFY(capturedRequests[1].contains(QStringLiteral("hdrMode=1")));
        QVERIFY(!capturedRequests[1].contains(QStringLiteral("sdsChromaMode")));
    }

private:
    void respond(QTcpServer& server, int& count, const QByteArray& response)
    {
        connect(&server, &QTcpServer::newConnection, &server, [&server, &count, response]() {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &count, response]() {
                if (socket->property("responded").toBool()) return;
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                if (!request.contains("\r\n\r\n")) {
                    socket->setProperty("request", request);
                    return;
                }
                socket->setProperty("responded", true);
                ++count;
                socket->write(response);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    QTemporaryDir m_Profile;
    QString m_Service;
};

int runNvHttpTests(int argc, char** argv)
{
    TestNvHttp tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_nvhttp.moc"
