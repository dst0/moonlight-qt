#pragma once

#include "lossless_codec.h"

#include <QByteArray>
#include <QMutex>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>

class SdsLosslessWorker;

class SdsLosslessClient
{
public:
    struct Config
    {
        QString host;
        quint16 httpsPort = 0;
        QSslCertificate serverCert;
        QSslCertificate clientCert;
        QSslKey clientKey;
        sds::lossless::hash_algo_e hashAlgo = sds::lossless::hash_algo_e::crc32c;
        int retryBaseMs = 50;
        int retryMaxMs = 2000;
    };

    struct FramePacket
    {
        sds::lossless::frame_header_t header {};
        QByteArray bytes;
        std::uint64_t receivedUs = 0;
    };

    SdsLosslessClient(const Config& config, std::function<void()> onFrameReady = {});
    ~SdsLosslessClient();

    SdsLosslessClient(const SdsLosslessClient&) = delete;
    SdsLosslessClient& operator=(const SdsLosslessClient&) = delete;

    void start();
    void wakeWaiters();
    void stop();

    void requestKeyframe();
    void setHashAlgo(sds::lossless::hash_algo_e algo);
    void sendControl(sds::lossless::control_msg_e msg, std::uint32_t arg = 0);

    bool popFrame(FramePacket& out, int timeoutMs = 0);
    std::size_t queuedFrameCount() const;
    bool isConnected() const;
    bool hadPinFailure() const;
    QString lastError() const;

private:
    friend class SdsLosslessWorker;

    bool tryPushFrame(FramePacket&& packet);
    void setConnectedState(bool connected);
    void setPinFailure(const QString& reason);

    QThread m_Thread;
    SdsLosslessWorker* m_Worker = nullptr;
    std::function<void()> m_OnFrameReady;

    mutable QMutex m_StateMutex;
    QWaitCondition m_FrameCond;
    std::deque<FramePacket> m_Queue;
    bool m_Started = false;
    bool m_Stopped = false;
    bool m_Connected = false;
    bool m_PinFailure = false;
    QString m_LastError;

    static constexpr std::size_t kMaxQueuedFrames = 4;
};
