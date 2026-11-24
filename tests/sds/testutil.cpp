#include "testutil.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTest>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <zlib.h>

namespace SdsTest {

namespace {

QByteArray bioToByteArray(BIO* bio)
{
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    return QByteArray(mem->data, static_cast<qsizetype>(mem->length));
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray be32(quint32 v)
{
    QByteArray b(4, Qt::Uninitialized);
    qToBigEndian(v, b.data());
    return b;
}

QByteArray pngChunk(const char* type, const QByteArray& data, bool corruptCrc = false)
{
    QByteArray typeAndData = QByteArray(type, 4) + data;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(typeAndData.constData()), static_cast<uInt>(typeAndData.size()));
    if (corruptCrc) {
        crc ^= 0xFFFFFFFFu;
    }
    return be32(static_cast<quint32>(data.size())) + typeAndData + be32(static_cast<quint32>(crc));
}

QByteArray ihdr(quint32 w, quint32 h, quint8 depth, quint8 colorType)
{
    QByteArray d = be32(w) + be32(h);
    d.append(static_cast<char>(depth));
    d.append(static_cast<char>(colorType));
    d.append('\0'); // compression
    d.append('\0'); // filter
    d.append('\0'); // interlace
    return d;
}

const QByteArray kPngSignature("\x89PNG\r\n\x1a\n", 8);

} // namespace

bool generateIdentity(const QString& dir, const QString& baseName, const QString& commonName, Identity* out)
{
    EVP_PKEY* pk = EVP_RSA_gen(2048);
    if (pk == nullptr) {
        return false;
    }

    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), static_cast<long>(QRandomGenerator::global()->bounded(1, 1 << 30)));
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 30);
    X509_set_pubkey(cert, pk);

    X509_NAME* name = X509_NAME_new();
    QByteArray cn = commonName.toUtf8();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn.constData()), -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);
    X509_sign(cert, pk, EVP_sha256());

    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, cert);
    QByteArray certPem = bioToByteArray(certBio);
    BIO_free(certBio);

    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey_traditional(keyBio, pk, nullptr, nullptr, 0, nullptr, nullptr);
    QByteArray keyPem = bioToByteArray(keyBio);
    BIO_free(keyBio);

    X509_free(cert);
    EVP_PKEY_free(pk);

    out->certPath = dir + QLatin1Char('/') + baseName + QStringLiteral(".pem");
    out->keyPath = dir + QLatin1Char('/') + baseName + QStringLiteral(".key");
    if (!writeFile(out->certPath, certPem) || !writeFile(out->keyPath, keyPem)) {
        return false;
    }
    out->cert = QSslCertificate(certPem);
    out->key = QSslKey(keyPem, QSsl::Rsa);
    return !out->cert.isNull() && !out->key.isNull();
}

QByteArray makePng(quint32 width, quint32 height)
{
    QByteArray raw;
    raw.reserve(static_cast<qsizetype>((width * 4 + 1) * height));
    for (quint32 y = 0; y < height; y++) {
        raw.append('\0'); // filter: none
        for (quint32 x = 0; x < width; x++) {
            raw.append(static_cast<char>(x * 37));
            raw.append(static_cast<char>(y * 91));
            raw.append(static_cast<char>((x + y) * 13));
            raw.append(static_cast<char>(0xFF));
        }
    }

    uLongf destLen = compressBound(static_cast<uLong>(raw.size()));
    QByteArray z(static_cast<qsizetype>(destLen), Qt::Uninitialized);
    compress(reinterpret_cast<Bytef*>(z.data()), &destLen, reinterpret_cast<const Bytef*>(raw.constData()),
             static_cast<uLong>(raw.size()));
    z.resize(static_cast<qsizetype>(destLen));

    return kPngSignature + pngChunk("IHDR", ihdr(width, height, 8, 6)) + pngChunk("IDAT", z) + pngChunk("IEND", QByteArray());
}

QByteArray makePngHeader(quint32 width, quint32 height, quint8 bitDepth, quint8 colorType, bool corruptCrc)
{
    return kPngSignature + pngChunk("IHDR", ihdr(width, height, bitDepth, colorType), corruptCrc) +
            pngChunk("IEND", QByteArray());
}

QByteArray randomBytes(int size)
{
    QByteArray b(size, Qt::Uninitialized);
    QRandomGenerator* g = QRandomGenerator::global();
    for (int i = 0; i < size; i++) {
        b[i] = static_cast<char>(g->bounded(256));
    }
    return b;
}

// ---- MockHost -------------------------------------------------------------------------------

MockHost::~MockHost()
{
    stop();
}

bool MockHost::start(const Identity& server, const Identity& trustedClient, QString* error)
{
    QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        python = QStringLiteral("/usr/bin/python3");
    }

    const QString script = QFINDTESTDATA("mock_sds_host.py");
    if (script.isEmpty()) {
        *error = QStringLiteral("cannot locate mock_sds_host.py for SDS integration tests");
        return false;
    }

    m_Proc.setProcessChannelMode(QProcess::SeparateChannels);
    m_Proc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    m_Proc.start(python, { QStringLiteral("-I"), script,
                           QStringLiteral("--cert"), server.certPath,
                           QStringLiteral("--key"), server.keyPath,
                           QStringLiteral("--client-cert"), trustedClient.certPath });
    if (!m_Proc.waitForStarted(10000)) {
        *error = QStringLiteral("cannot start %1: %2").arg(python, m_Proc.errorString());
        return false;
    }

    QByteArray line = readLine(15000);
    QJsonObject ready = QJsonDocument::fromJson(line).object();
    if (!ready.value(QStringLiteral("ready")).toBool()) {
        *error = QStringLiteral("mock host did not start: %1 %2").arg(QString::fromUtf8(line),
                                                                     QString::fromUtf8(m_Proc.readAllStandardError()));
        return false;
    }
    m_Port = static_cast<quint16>(ready.value(QStringLiteral("port")).toInt());
    return m_Port != 0;
}

void MockHost::stop()
{
    if (m_Proc.state() != QProcess::NotRunning) {
        m_Proc.write("{\"cmd\":\"quit\"}\n");
        m_Proc.closeWriteChannel();
        if (!m_Proc.waitForFinished(3000)) {
            m_Proc.kill();
            m_Proc.waitForFinished(3000);
        }
    }
}

QByteArray MockHost::readLine(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!m_Proc.canReadLine()) {
        int left = timeoutMs - static_cast<int>(timer.elapsed());
        if (left <= 0 || m_Proc.state() == QProcess::NotRunning) {
            return QByteArray();
        }
        m_Proc.waitForReadyRead(qMin(left, 200));
    }
    return m_Proc.readLine().trimmed();
}

QJsonObject MockHost::cmd(const QJsonObject& command)
{
    m_Proc.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + "\n");
    m_Proc.waitForBytesWritten(3000);
    return QJsonDocument::fromJson(readLine(10000)).object();
}

QJsonObject MockHost::config(const QJsonObject& settings)
{
    QJsonObject c = settings;
    c.insert(QStringLiteral("cmd"), QStringLiteral("config"));
    return cmd(c);
}

QJsonObject MockHost::fault(const QJsonObject& fault)
{
    QJsonObject c = fault;
    c.insert(QStringLiteral("cmd"), QStringLiteral("fault"));
    return cmd(c);
}

QList<QJsonObject> MockHost::log()
{
    QList<QJsonObject> entries;
    const QJsonArray arr = cmd({ { QStringLiteral("cmd"), QStringLiteral("log") } }).value(QStringLiteral("log")).toArray();
    for (const QJsonValue& v : arr) {
        entries.append(v.toObject());
    }
    return entries;
}

QJsonObject MockHost::state()
{
    return cmd({ { QStringLiteral("cmd"), QStringLiteral("state") } });
}

// ---- EventCollector -------------------------------------------------------------------------

void EventCollector::onSdsClientEvent(const Sds::ClientEvent& event)
{
    QMutexLocker lock(&m_Lock);
    m_Events.append(event);
}

QList<Sds::ClientEvent> EventCollector::events() const
{
    QMutexLocker lock(&m_Lock);
    return m_Events;
}

int EventCollector::count(const Predicate& pred) const
{
    QMutexLocker lock(&m_Lock);
    int n = 0;
    for (const auto& e : m_Events) {
        if (pred(e)) {
            n++;
        }
    }
    return n;
}

bool EventCollector::waitFor(const Predicate& pred, int timeoutMs) const
{
    return QTest::qWaitFor([&]() { return count(pred) > 0; }, timeoutMs);
}

bool EventCollector::hasNoticeContaining(const QString& text) const
{
    return count(noticeContaining(text)) > 0;
}

void EventCollector::clear()
{
    QMutexLocker lock(&m_Lock);
    m_Events.clear();
}

EventCollector::Predicate EventCollector::ofType(Sds::ClientEvent::Type type)
{
    return [type](const Sds::ClientEvent& e) { return e.type == type; };
}

EventCollector::Predicate EventCollector::noticeContaining(const QString& text)
{
    return [text](const Sds::ClientEvent& e) {
        return (e.type == Sds::ClientEvent::Notice || e.type == Sds::ClientEvent::Stopped ||
                e.type == Sds::ClientEvent::TransferFinished) &&
                e.message.contains(text, Qt::CaseInsensitive);
    };
}

} // namespace SdsTest
