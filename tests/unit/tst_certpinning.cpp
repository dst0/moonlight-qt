#include <QtTest>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSslEllipticCurve>
#include <QSslServer>
#include <QSslSocket>
#include <functional>

#include "backend/certpinning.h"
#include "testcerts.h"
#include "tests.h"

// Minimal HTTPS server on 127.0.0.1 that presents a given certificate and counts
// the application data it receives.
class TlsTestServer : public QObject
{
    Q_OBJECT

public:
    // issuer: if set, its certificate is sent after the leaf as part of the chain
    explicit TlsTestServer(const TestCert& identity, const TestCert* issuer = nullptr,
                           std::function<void(QSslConfiguration&)> customize = {})
    {
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        if (issuer != nullptr) {
            config.setLocalCertificateChain({identity.cert(), issuer->cert()});
        }
        else {
            config.setLocalCertificate(identity.cert());
        }
        config.setPrivateKey(identity.key());
        config.setPeerVerifyMode(QSslSocket::VerifyNone);
        if (customize) {
            customize(config);
        }
        m_Server.setSslConfiguration(config);
        connect(&m_Server, &QSslServer::pendingConnectionAvailable, this, &TlsTestServer::acceptPending);
        listening = m_Server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const { return m_Server.serverPort(); }

    bool listening = false;
    int requestCount = 0;
    qint64 bytesReceived = 0;

private slots:
    void acceptPending()
    {
        while (QTcpSocket* socket = m_Server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                QByteArray data = socket->readAll();
                bytesReceived += data.size();
                m_Buffer += data;
                if (m_Buffer.contains("\r\n\r\n")) {
                    m_Buffer.clear();
                    requestCount++;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
                    socket->disconnectFromHost();
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

private:
    QSslServer m_Server;
    QByteArray m_Buffer;
};

namespace {

struct FetchResult
{
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QByteArray body;
    bool rejectedByPin = false;
    bool peerMatchesPin = false;
    bool finished = false;
};

// Performs a GET like NvHTTP::openConnection(), optionally enforcing a pin on the reply
FetchResult fetch(quint16 port, const QSslConfiguration& config, const QSslCertificate* enforcePin,
                  const QSslCertificate& checkPin)
{
    QNetworkAccessManager nam;
    nam.setProxy(QNetworkProxy::NoProxy);

    QNetworkRequest request(QUrl(QStringLiteral("https://127.0.0.1:%1/serverinfo?uniqueid=0123456789ABCDEF").arg(port)));
    request.setSslConfiguration(config);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkReply* reply = nam.get(request);
    if (enforcePin != nullptr) {
        CertPinning::enforce(reply, *enforcePin);
    }

    FetchResult result;
    QSignalSpy finishedSpy(reply, &QNetworkReply::finished);
    result.finished = reply->isFinished() || finishedSpy.wait(15000);
    result.error = reply->error();
    result.body = reply->readAll();
    result.rejectedByPin = CertPinning::wasRejected(reply);
    result.peerMatchesPin = CertPinning::matchesPinned(checkPin, reply->sslConfiguration().peerCertificate());
    delete reply;

    // Give any request bytes that might have been sent time to reach the server
    QTest::qWait(300);
    return result;
}

}

class TestCertPinning : public QObject
{
    Q_OBJECT

    TestCert m_Pinned;
    TestCert m_Other;
    TestCert m_Ca;
    TestCert m_CaSigned;

private slots:
    void initTestCase()
    {
        qInfo() << "TLS backend:" << QSslSocket::activeBackend() << QSslSocket::sslLibraryVersionString();
        QVERIFY(QSslSocket::supportsSsl());

        m_Pinned = TestCerts::makeSelfSigned("Pinned Test Host");
        m_Other = TestCerts::makeSelfSigned("Impostor Test Host");
        m_Ca = TestCerts::makeCa("Test Root CA");
        m_CaSigned = TestCerts::makeLocalhostServerCert(m_Ca, "CA-signed Test Host");

        QVERIFY(!m_Pinned.cert().isNull() && !m_Pinned.key().isNull());
        QVERIFY(!m_Other.cert().isNull() && !m_Other.key().isNull());
        QVERIFY(!m_Ca.cert().isNull());
        QVERIFY(!m_CaSigned.cert().isNull() && !m_CaSigned.key().isNull());
    }

    // --- Pure comparison functions ---

    void matchesPinned()
    {
        QSslCertificate pinned = m_Pinned.cert();
        QVERIFY(CertPinning::matchesPinned(pinned, pinned));
        // Same certificate re-parsed from DER
        QVERIFY(CertPinning::matchesPinned(pinned, QSslCertificate(pinned.toDer(), QSsl::Der)));
        QVERIFY(!CertPinning::matchesPinned(pinned, m_Other.cert()));
        QVERIFY(!CertPinning::matchesPinned(pinned, m_CaSigned.cert()));
        QVERIFY(!CertPinning::matchesPinned(pinned, QSslCertificate()));
        QVERIFY(!CertPinning::matchesPinned(QSslCertificate(), pinned));
        QVERIFY(!CertPinning::matchesPinned(QSslCertificate(), QSslCertificate()));
    }

    void errorsConcernOnlyPinned()
    {
        QSslCertificate pinned = m_Pinned.cert();
        QSslCertificate other = m_Other.cert();

        QList<QSslError> pinnedErrors{
            QSslError(QSslError::SelfSignedCertificate, pinned),
            QSslError(QSslError::HostNameMismatch, pinned),
        };
        QVERIFY(CertPinning::errorsConcernOnlyPinned(pinned, pinnedErrors));

        // Nothing to vouch for, or no pin at all
        QVERIFY(!CertPinning::errorsConcernOnlyPinned(pinned, {}));
        QVERIFY(!CertPinning::errorsConcernOnlyPinned(QSslCertificate(), pinnedErrors));

        // Any error about another certificate (e.g. an intermediate) poisons the list
        QList<QSslError> mixed = pinnedErrors;
        mixed.append(QSslError(QSslError::UnableToGetLocalIssuerCertificate, other));
        QVERIFY(!CertPinning::errorsConcernOnlyPinned(pinned, mixed));
        QVERIFY(!CertPinning::errorsConcernOnlyPinned(pinned, {QSslError(QSslError::SelfSignedCertificate, other)}));

        // Errors without a certificate are never ignored
        QVERIFY(!CertPinning::errorsConcernOnlyPinned(pinned, {QSslError(QSslError::NoPeerCertificate)}));
        QVERIFY(!CertPinning::errorsConcernOnlyPinned(pinned, {QSslError(QSslError::HostNameMismatch, pinned),
                                                               QSslError(QSslError::UnspecifiedError)}));
    }

    void restrictToPinned()
    {
        QSslConfiguration base = QSslConfiguration::defaultConfiguration();
        base.setLocalCertificate(m_Other.cert());
        base.setPrivateKey(m_Other.key());
        base.addCaCertificate(m_Ca.cert());

        QSslConfiguration restricted = CertPinning::restrictToPinned(base, m_Pinned.cert());
        QCOMPARE(restricted.caCertificates(), QList<QSslCertificate>{m_Pinned.cert()});
        QCOMPARE(restricted.localCertificate(), m_Other.cert());
        QCOMPARE(restricted.privateKey(), m_Other.key());
        QCOMPARE(restricted.peerVerifyMode(), base.peerVerifyMode());
        QCOMPARE(restricted.protocol(), QSsl::TlsV1_3OrLater);
        QVERIFY(restricted.ellipticCurves().isEmpty());
        QCOMPARE(restricted.backendConfiguration().value("Groups").toString(), QStringLiteral("X25519MLKEM768"));

        // The base configuration is not modified
        QVERIFY(base.caCertificates().contains(m_Ca.cert()));

        // A null pin trusts nothing
        QVERIFY(CertPinning::restrictToPinned(base, QSslCertificate()).caCertificates().isEmpty());
    }

    // --- Real TLS handshakes against a local server ---

    void pinnedSelfSignedHostIsAccepted()
    {
        TlsTestServer server(m_Pinned);
        QVERIFY(server.listening);

        QSslCertificate pin = m_Pinned.cert();
        FetchResult r = fetch(server.port(), CertPinning::restrictToPinned(QSslConfiguration::defaultConfiguration(), pin),
                              &pin, pin);
        QVERIFY(r.finished);
        QCOMPARE(r.error, QNetworkReply::NoError);
        QCOMPARE(r.body, QByteArray("ok"));
        QVERIFY(!r.rejectedByPin);
        QVERIFY(r.peerMatchesPin);
        QCOMPARE(server.requestCount, 1);
    }

    void otherSelfSignedHostIsRejectedBeforeRequest()
    {
        TlsTestServer server(m_Other);
        QVERIFY(server.listening);

        QSslCertificate pin = m_Pinned.cert();
        FetchResult r = fetch(server.port(), CertPinning::restrictToPinned(QSslConfiguration::defaultConfiguration(), pin),
                              &pin, pin);
        QVERIFY(r.finished);
        QCOMPARE(r.error, QNetworkReply::SslHandshakeFailedError);
        QVERIFY(r.body.isEmpty());
        QCOMPARE(server.requestCount, 0);
        QCOMPARE(server.bytesReceived, 0);
    }

    // Control for the next test: the CA-signed host is valid for a client that trusts the CA,
    // so the TLS stack itself raises no error (the situation the old code never pinned).
    void caSignedHostIsTrustedWithoutPinning()
    {
        TlsTestServer server(m_CaSigned);
        QVERIFY(server.listening);

        QSslConfiguration trustsCa = QSslConfiguration::defaultConfiguration();
        trustsCa.addCaCertificate(m_Ca.cert());

        FetchResult r = fetch(server.port(), trustsCa, nullptr, m_CaSigned.cert());
        QVERIFY(r.finished);
        QCOMPARE(r.error, QNetworkReply::NoError);
        QCOMPARE(server.requestCount, 1);
    }

    void caSignedHostIsRejectedByPin()
    {
        TlsTestServer server(m_CaSigned, &m_Ca);
        QVERIFY(server.listening);

        QSslConfiguration trustsCa = QSslConfiguration::defaultConfiguration();
        trustsCa.addCaCertificate(m_Ca.cert());

        QSslCertificate pin = m_Pinned.cert();
        FetchResult r = fetch(server.port(), CertPinning::restrictToPinned(trustsCa, pin), &pin, pin);
        QVERIFY(r.finished);
        QVERIFY(r.error != QNetworkReply::NoError);
        QVERIFY(r.body.isEmpty());
        QCOMPARE(server.requestCount, 0);
        QCOMPARE(server.bytesReceived, 0);
    }

    // A host whose pinned leaf certificate is CA-issued and sent with its issuer chain
    void pinnedLeafWithIssuerChainIsAccepted()
    {
        TlsTestServer server(m_CaSigned, &m_Ca);
        QVERIFY(server.listening);

        QSslCertificate pin = m_CaSigned.cert();
        FetchResult r = fetch(server.port(), CertPinning::restrictToPinned(QSslConfiguration::defaultConfiguration(), pin),
                              &pin, pin);
        QVERIFY(r.finished);
        QCOMPARE(r.error, QNetworkReply::NoError);
        QCOMPARE(r.body, QByteArray("ok"));
        QVERIFY(r.peerMatchesPin);
        QCOMPARE(server.requestCount, 1);
    }

    // The encrypted() hook alone: the handshake succeeds without any TLS error, but the
    // peer is not the pinned host, so the reply must be aborted before the request is sent.
    void encryptedHookAbortsBeforeRequest()
    {
        TlsTestServer server(m_CaSigned);
        QVERIFY(server.listening);

        QSslConfiguration trustsCa = QSslConfiguration::defaultConfiguration();
        trustsCa.addCaCertificate(m_Ca.cert());

        QSslCertificate pin = m_Pinned.cert();
        FetchResult r = fetch(server.port(), trustsCa, &pin, pin);
        QVERIFY(r.finished);
        QCOMPARE(r.error, QNetworkReply::OperationCanceledError);
        QVERIFY(r.rejectedByPin);
        QVERIFY(r.body.isEmpty());
        QCOMPARE(server.requestCount, 0);
        QCOMPARE(server.bytesReceived, 0);
    }

    void pqTls13GroupPolicy()
    {
        QSslCertificate pin = m_Pinned.cert();
        const QSslConfiguration clientConfig =
                CertPinning::restrictToPinned(QSslConfiguration::defaultConfiguration(), pin);

        // Server offering TLS 1.3 + X25519MLKEM768 succeeds
        {
            TlsTestServer pqServer(m_Pinned, nullptr, [](QSslConfiguration& cfg) {
                cfg.setProtocol(QSsl::TlsV1_3OrLater);
                cfg.setEllipticCurves(QVector<QSslEllipticCurve>());
                cfg.setBackendConfigurationOption("Groups", "X25519MLKEM768");
            });
            QVERIFY(pqServer.listening);
            FetchResult r = fetch(pqServer.port(), clientConfig, &pin, pin);
            QVERIFY(r.finished);
            QCOMPARE(r.error, QNetworkReply::NoError);
            QCOMPARE(r.body, QByteArray("ok"));
            QCOMPARE(pqServer.requestCount, 1);
        }

        // Server restricted to TLS 1.2 is rejected
        {
            TlsTestServer tls12Server(m_Pinned, nullptr, [](QSslConfiguration& cfg) {
                cfg.setProtocol(QSsl::TlsV1_2);
            });
            QVERIFY(tls12Server.listening);
            FetchResult r = fetch(tls12Server.port(), clientConfig, &pin, pin);
            QVERIFY(r.finished);
            QVERIFY(r.error != QNetworkReply::NoError);
            QVERIFY(r.body.isEmpty());
            QCOMPARE(tls12Server.requestCount, 0);
            QCOMPARE(tls12Server.bytesReceived, 0);
        }

        // Server offering only classical groups (no X25519MLKEM768) is rejected
        {
            TlsTestServer classicalServer(m_Pinned, nullptr, [](QSslConfiguration& cfg) {
                cfg.setProtocol(QSsl::TlsV1_3OrLater);
                cfg.setEllipticCurves(QVector<QSslEllipticCurve>());
                cfg.setBackendConfigurationOption("Groups", "X25519:P-256");
            });
            QVERIFY(classicalServer.listening);
            FetchResult r = fetch(classicalServer.port(), clientConfig, &pin, pin);
            QVERIFY(r.finished);
            QVERIFY(r.error != QNetworkReply::NoError);
            QVERIFY(r.body.isEmpty());
            QCOMPARE(classicalServer.requestCount, 0);
            QCOMPARE(classicalServer.bytesReceived, 0);
        }
    }
};

int runCertPinningTests(int argc, char** argv)
{
    TestCertPinning tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_certpinning.moc"
