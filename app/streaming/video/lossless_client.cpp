#include "lossless_client.h"

#include "backend/certpinning.h"
#include "backend/lanpolicy.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHostAddress>
#include <QMetaObject>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace {

bool equalsIgnoreCase(const QByteArray& a, const char* b)
{
    return QByteArray(a).toLower() == QByteArray(b).toLower();
}

std::uint64_t maxPlausiblePayloadBytes(std::uint32_t width, std::uint32_t height)
{
    const std::uint64_t tilesX = (static_cast<std::uint64_t>(width) + sds::lossless::TILE_SIZE - 1u) / sds::lossless::TILE_SIZE;
    const std::uint64_t tilesY = (static_cast<std::uint64_t>(height) + sds::lossless::TILE_SIZE - 1u) / sds::lossless::TILE_SIZE;
    const std::uint64_t numTiles = tilesX * tilesY;
    const std::uint64_t skipBytes = (numTiles + 7u) / 8u;
    const std::uint64_t copyRectBytes = sds::lossless::MAX_COPY_RECTS * 16u;
    // Allow minor slack beyond worst-case raw tiles + per-tile 8B header
    return copyRectBytes + skipBytes + numTiles * (8u + sds::lossless::MAX_TILE_PAYLOAD) + 4096u;
}

} // namespace

class SdsLosslessWorker : public QObject
{
    Q_OBJECT
public:
    SdsLosslessWorker(SdsLosslessClient* owner, SdsLosslessClient::Config cfg)
        : m_Owner(owner)
        , m_Cfg(std::move(cfg))
    {
    }

    ~SdsLosslessWorker() override
    {
        cleanupSocket();
    }

public slots:
    void startWorker()
    {
        if (m_Stopped) {
            return;
        }
        m_RetryTimer = new QTimer(this);
        m_RetryTimer->setSingleShot(true);
        connect(m_RetryTimer, &QTimer::timeout, this, &SdsLosslessWorker::connectOnce);

        QHostAddress addr;
        if (!addr.setAddress(m_Cfg.host) || !LanPolicy::isPermittedConnectionTarget(addr)) {
            m_Owner->setPinFailure(QStringLiteral("Refused non-LAN / non-literal host: %1").arg(m_Cfg.host));
            return;
        }
        if (m_Cfg.serverCert.isNull() || m_Cfg.clientCert.isNull() || m_Cfg.clientKey.isNull() || m_Cfg.httpsPort == 0) {
            m_Owner->setPinFailure(QStringLiteral("Missing paired TLS certificates or port for lossless stream"));
            return;
        }

        m_RetryDelayMs = std::max(25, m_Cfg.retryBaseMs);
        connectOnce();
    }

    void stopWorker()
    {
        m_Stopped = true;
        if (m_RetryTimer) {
            m_RetryTimer->stop();
        }
        cleanupSocket();
    }

    void sendControlSlot(quint8 msgType, quint32 arg)
    {
        if (!m_Socket || !m_Upgraded || m_Socket->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        std::uint8_t buf[sds::lossless::CONTROL_MSG_SIZE] {};
        sds::lossless::serialize_control_msg(
            static_cast<sds::lossless::control_msg_e>(msgType),
            arg,
            buf
        );
        m_Socket->write(reinterpret_cast<const char*>(buf), sizeof(buf));
        m_Socket->flush();
    }

    void resumeReading()
    {
        if (m_Stopped || !m_ReadPaused) {
            return;
        }
        m_ReadPaused = false;
        drainSocket();
    }

private slots:
    void connectOnce()
    {
        if (m_Stopped) {
            return;
        }
        cleanupSocket();

        m_Upgraded = false;
        m_ReadPaused = false;
        m_RxBuffer.clear();

        m_Socket = new QSslSocket(this);
        m_Socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        m_Socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_Socket->setReadBufferSize(4 * 1024 * 1024);

        QSslConfiguration sslCfg = CertPinning::restrictToPinned(QSslConfiguration::defaultConfiguration(), m_Cfg.serverCert);
        sslCfg.setLocalCertificate(m_Cfg.clientCert);
        sslCfg.setPrivateKey(m_Cfg.clientKey);
        sslCfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
        m_Socket->setSslConfiguration(sslCfg);

        connect(m_Socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError>& errors) {
            if (!m_Socket) {
                return;
            }
            if (CertPinning::errorsConcernOnlyPinned(m_Cfg.serverCert, errors) ||
                CertPinning::matchesPinned(m_Cfg.serverCert, m_Socket->peerCertificate())) {
                m_Socket->ignoreSslErrors(errors);
            } else {
                m_Owner->setPinFailure(QStringLiteral("TLS certificate pin mismatch"));
                m_Socket->abort();
            }
        });

        connect(m_Socket, &QSslSocket::encrypted, this, [this]() {
            if (!m_Socket || m_Stopped) {
                return;
            }
            if (!CertPinning::matchesPinned(m_Cfg.serverCert, m_Socket->peerCertificate())) {
                m_Owner->setPinFailure(QStringLiteral("Peer certificate does not match pinned host certificate"));
                m_Socket->abort();
                return;
            }
            m_RetryDelayMs = std::max(25, m_Cfg.retryBaseMs);
            const QString hostHeader = (m_Cfg.host.contains(':') && !m_Cfg.host.startsWith('['))
                ? (QStringLiteral("[") + m_Cfg.host + QStringLiteral("]"))
                : m_Cfg.host;
            const QByteArray req =
                "GET /sds/v1/lossless-stream HTTP/1.1\r\n"
                "Host: " + hostHeader.toUtf8() + ":" + QByteArray::number(m_Cfg.httpsPort) + "\r\n"
                "Upgrade: sds-lossless-v1\r\n"
                "Connection: Upgrade\r\n"
                "\r\n";
            m_Socket->write(req);
            m_Socket->flush();
        });

        connect(m_Socket, &QSslSocket::readyRead, this, &SdsLosslessWorker::drainSocket);
        connect(m_Socket, &QSslSocket::disconnected, this, &SdsLosslessWorker::handleSocketClosed);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(m_Socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            handleSocketClosed();
        });
#else
        connect(m_Socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, [this](QAbstractSocket::SocketError) {
            handleSocketClosed();
        });
#endif

        m_Socket->connectToHostEncrypted(m_Cfg.host, m_Cfg.httpsPort);
    }

    void handleSocketClosed()
    {
        if (m_Socket && m_Socket->error() != QAbstractSocket::UnknownSocketError) {
            QMutexLocker lk(&m_Owner->m_StateMutex);
            m_Owner->m_LastError = m_Socket->errorString();
        }
        m_Owner->setConnectedState(false);
        cleanupSocket();
        scheduleReconnect();
    }

    void drainSocket()
    {
        if (!m_Socket || m_Stopped || m_ReadPaused) {
            return;
        }

        if (m_Socket->bytesAvailable() > 0) {
            m_RxBuffer.append(m_Socket->readAll());
        }

        if (!m_Upgraded) {
            const int headerEnd = m_RxBuffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                if (m_RxBuffer.size() > 8192) {
                    abortAndReconnect(QStringLiteral("Oversized HTTP upgrade response"));
                }
                return;
            }

            const QByteArray head = m_RxBuffer.left(headerEnd);
            m_RxBuffer.remove(0, headerEnd + 4);

            const QList<QByteArray> lines = head.split('\n');
            if (lines.isEmpty()) {
                abortAndReconnect(QStringLiteral("Empty HTTP upgrade response"));
                return;
            }
            const QByteArray statusLine = lines.first().trimmed();
            if (!statusLine.startsWith("HTTP/1.1 101")) {
                abortAndReconnect(QStringLiteral("Unexpected upgrade status: %1").arg(QString::fromLatin1(statusLine)));
                return;
            }

            bool hasUpgradeHeader = false;
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines[i].trimmed();
                const int colon = line.indexOf(':');
                if (colon <= 0) {
                    continue;
                }
                const QByteArray name = line.left(colon).trimmed();
                const QByteArray val = line.mid(colon + 1).trimmed();
                if (equalsIgnoreCase(name, "Upgrade") && equalsIgnoreCase(val, "sds-lossless-v1")) {
                    hasUpgradeHeader = true;
                }
            }
            if (!hasUpgradeHeader) {
                abortAndReconnect(QStringLiteral("Missing Upgrade: sds-lossless-v1 header"));
                return;
            }

            m_Upgraded = true;
            m_Owner->setConnectedState(true);

            // Sync hash mode & trigger immediate clean keyframe
            if (m_Cfg.hashAlgo != sds::lossless::hash_algo_e::crc32c) {
                sendControlSlot(static_cast<quint8>(sds::lossless::control_msg_e::set_hash_algo),
                                static_cast<quint32>(m_Cfg.hashAlgo));
            }
            sendControlSlot(static_cast<quint8>(sds::lossless::control_msg_e::request_keyframe), 0);
        }

        while (m_RxBuffer.size() >= static_cast<int>(sds::lossless::HEADER_SIZE)) {
            sds::lossless::frame_header_t hdr {};
            if (!sds::lossless::parse_frame_header(
                    reinterpret_cast<const std::uint8_t*>(m_RxBuffer.constData()),
                    static_cast<std::size_t>(m_RxBuffer.size()),
                    hdr)) {
                abortAndReconnect(QStringLiteral("Corrupt SDLL frame header"));
                return;
            }

            const std::uint64_t maxPayload = maxPlausiblePayloadBytes(hdr.width, hdr.height);
            if (hdr.payload_len > maxPayload || hdr.payload_len > (512ull * 1024ull * 1024ull)) {
                abortAndReconnect(QStringLiteral("Implausible SDLL payload length"));
                return;
            }

            const std::uint64_t totalPacketSize = sds::lossless::HEADER_SIZE + hdr.payload_len;
            if (static_cast<std::uint64_t>(m_RxBuffer.size()) < totalPacketSize) {
                break;
            }

            SdsLosslessClient::FramePacket pkt;
            pkt.header = hdr;
            pkt.bytes = m_RxBuffer.left(static_cast<int>(totalPacketSize));
            pkt.receivedUs = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()) * 1000ull;

            if (!m_Owner->tryPushFrame(std::move(pkt))) {
                // Queue is full: pause consuming from QSslSocket so TCP backpressure propagates cleanly.
                m_ReadPaused = true;
                return;
            }

            m_RxBuffer.remove(0, static_cast<int>(totalPacketSize));
        }
        if (m_RxBuffer.isEmpty() && m_RxBuffer.capacity() > 16 * 1024 * 1024) {
            m_RxBuffer.squeeze();
        }
    }

private:
    void cleanupSocket()
    {
        if (m_Socket) {
            m_Socket->disconnect(this);
            m_Socket->abort();
            m_Socket->deleteLater();
            m_Socket = nullptr;
        }
        m_Upgraded = false;
    }

    void abortAndReconnect(const QString& reason)
    {
        {
            QMutexLocker lk(&m_Owner->m_StateMutex);
            m_Owner->m_LastError = reason;
        }
        m_Owner->setConnectedState(false);
        cleanupSocket();
        scheduleReconnect();
    }

    void scheduleReconnect()
    {
        if (m_Stopped || m_Owner->hadPinFailure()) {
            return;
        }
        if (m_RetryTimer && !m_RetryTimer->isActive()) {
            m_RetryTimer->start(m_RetryDelayMs);
            m_RetryDelayMs = std::min(m_Cfg.retryMaxMs, std::max(50, m_RetryDelayMs * 2));
        }
    }

    SdsLosslessClient* m_Owner = nullptr;
    SdsLosslessClient::Config m_Cfg;
    QSslSocket* m_Socket = nullptr;
    QTimer* m_RetryTimer = nullptr;
    QByteArray m_RxBuffer;
    bool m_Stopped = false;
    bool m_Upgraded = false;
    bool m_ReadPaused = false;
    int m_RetryDelayMs = 50;
};

SdsLosslessClient::SdsLosslessClient(const Config& config, std::function<void()> onFrameReady)
    : m_OnFrameReady(std::move(onFrameReady))
{
    m_Worker = new SdsLosslessWorker(this, config);
    m_Worker->moveToThread(&m_Thread);
    QObject::connect(&m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
}

SdsLosslessClient::~SdsLosslessClient()
{
    stop();
}

void SdsLosslessClient::start()
{
    {
        QMutexLocker lk(&m_StateMutex);
        if (m_Started || m_Stopped) {
            return;
        }
        m_Started = true;
    }
    m_Thread.start();
    QMetaObject::invokeMethod(m_Worker, "startWorker", Qt::QueuedConnection);
}

void SdsLosslessClient::stop()
{
    {
        QMutexLocker lk(&m_StateMutex);
        if (m_Stopped) {
            return;
        }
        m_Stopped = true;
        m_Queue.clear();
        m_FrameCond.wakeAll();
    }
    if (m_Thread.isRunning()) {
        QMetaObject::invokeMethod(m_Worker, "stopWorker", Qt::BlockingQueuedConnection);
        m_Thread.quit();
        m_Thread.wait();
    } else if (m_Worker) {
        delete m_Worker;
        m_Worker = nullptr;
    }
}

void SdsLosslessClient::requestKeyframe()
{
    sendControl(sds::lossless::control_msg_e::request_keyframe, 0);
}

void SdsLosslessClient::setHashAlgo(sds::lossless::hash_algo_e algo)
{
    sendControl(sds::lossless::control_msg_e::set_hash_algo, static_cast<std::uint32_t>(algo));
}

void SdsLosslessClient::sendControl(sds::lossless::control_msg_e msg, std::uint32_t arg)
{
    if (!m_Thread.isRunning() || !m_Worker) {
        return;
    }
    QMetaObject::invokeMethod(
        m_Worker,
        "sendControlSlot",
        Qt::QueuedConnection,
        Q_ARG(quint8, static_cast<quint8>(msg)),
        Q_ARG(quint32, arg)
    );
}

bool SdsLosslessClient::tryPushFrame(FramePacket&& packet)
{
    {
        QMutexLocker lk(&m_StateMutex);
        if (m_Stopped || m_Queue.size() >= kMaxQueuedFrames) {
            return false;
        }
        m_Queue.push_back(std::move(packet));
        m_FrameCond.wakeOne();
    }
    if (m_OnFrameReady) {
        m_OnFrameReady();
    }
    return true;
}

bool SdsLosslessClient::popFrame(FramePacket& out, int timeoutMs)
{
    bool shouldResume = false;
    {
        QMutexLocker lk(&m_StateMutex);
        if (m_Queue.empty() && timeoutMs > 0 && !m_Stopped) {
            m_FrameCond.wait(&m_StateMutex, timeoutMs);
        }
        if (m_Queue.empty()) {
            return false;
        }
        shouldResume = (m_Queue.size() >= kMaxQueuedFrames);
        out = std::move(m_Queue.front());
        m_Queue.pop_front();
    }
    if (shouldResume && m_Thread.isRunning() && m_Worker) {
        QMetaObject::invokeMethod(m_Worker, "resumeReading", Qt::QueuedConnection);
    }
    return true;
}

std::size_t SdsLosslessClient::queuedFrameCount() const
{
    QMutexLocker lk(&m_StateMutex);
    return m_Queue.size();
}

bool SdsLosslessClient::isConnected() const
{
    QMutexLocker lk(&m_StateMutex);
    return m_Connected;
}

bool SdsLosslessClient::hadPinFailure() const
{
    QMutexLocker lk(&m_StateMutex);
    return m_PinFailure;
}

QString SdsLosslessClient::lastError() const
{
    QMutexLocker lk(&m_StateMutex);
    return m_LastError;
}

void SdsLosslessClient::setConnectedState(bool connected)
{
    QMutexLocker lk(&m_StateMutex);
    m_Connected = connected;
}

void SdsLosslessClient::setPinFailure(const QString& reason)
{
    QMutexLocker lk(&m_StateMutex);
    m_PinFailure = true;
    m_LastError = reason;
    m_Connected = false;
}

#include "lossless_client.moc"
