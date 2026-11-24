#pragma once

// Helpers for the secure-desktop-stream client tests.

#include "streaming/sds/sdstypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>

#include <functional>

namespace SdsTest {

struct Identity
{
    QString certPath;
    QString keyPath;
    QSslCertificate cert;
    QSslKey key;
};

// Self-signed RSA-2048 certificate without extensions, like the ones Moonlight and Sunshine
// generate. The key is written in the traditional (PKCS#1) PEM format.
bool generateIdentity(const QString& dir, const QString& baseName, const QString& commonName, Identity* out);

// A complete, decodable RGBA PNG (zlib-compressed IDAT).
QByteArray makePng(quint32 width, quint32 height);

// Signature + IHDR (+ IEND) only, with a correct CRC unless corruptCrc is set.
QByteArray makePngHeader(quint32 width, quint32 height, quint8 bitDepth = 8, quint8 colorType = 6,
                         bool corruptCrc = false);

QByteArray randomBytes(int size);

// Runs tests/sds/mock_sds_host.py with `python3 -I` and talks to its stdin/stdout control
// channel.
class MockHost
{
public:
    ~MockHost();

    bool start(const Identity& server, const Identity& trustedClient, QString* error);
    void stop();

    quint16 port() const { return m_Port; }

    QJsonObject cmd(const QJsonObject& command);

    // Convenience wrappers
    QJsonObject config(const QJsonObject& settings);
    QJsonObject fault(const QJsonObject& fault);
    QList<QJsonObject> log();
    QJsonObject state();

private:
    QByteArray readLine(int timeoutMs);

    QProcess m_Proc;
    quint16 m_Port = 0;
};

// Thread-safe ClientListener that records every event.
class EventCollector : public Sds::ClientListener
{
public:
    using Predicate = std::function<bool(const Sds::ClientEvent&)>;

    void onSdsClientEvent(const Sds::ClientEvent& event) override;

    QList<Sds::ClientEvent> events() const;
    int count(const Predicate& pred) const;
    bool waitFor(const Predicate& pred, int timeoutMs = 15000) const;
    bool hasNoticeContaining(const QString& text) const;
    void clear();

    static Predicate ofType(Sds::ClientEvent::Type type);
    static Predicate noticeContaining(const QString& text);

private:
    mutable QMutex m_Lock;
    QList<Sds::ClientEvent> m_Events;
};

} // namespace SdsTest
