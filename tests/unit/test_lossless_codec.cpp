#include <QtTest>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "streaming/video/lossless_client.h"
#include "streaming/video/lossless_codec.h"
#include "streaming/video/lossless_hash.h"
#include "testcerts.h"
#include "tests.h"

#include <QHostAddress>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpSocket>

namespace {

std::vector<uint8_t> makeSolidBgra(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha = 0xAA)
{
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = b;
        buf[i + 1] = g;
        buf[i + 2] = r;
        buf[i + 3] = static_cast<uint8_t>(alpha ^ (i & 0xFFu));
    }
    return buf;
}

void fillRectBgra(std::vector<uint8_t>& buf, int w, int rx, int ry, int rw, int rh,
                  uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = ry; y < ry + rh; ++y) {
        uint8_t* row = buf.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
        for (int x = rx; x < rx + rw; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
}

void assertRgb24Equal(const uint8_t* a, int aStride, const uint8_t* b, int bStride, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = a + static_cast<size_t>(y) * aStride;
        const uint8_t* rb = b + static_cast<size_t>(y) * bStride;
        for (int x = 0; x < w; ++x) {
            QCOMPARE(ra[x * 4 + 0], rb[x * 4 + 0]);
            QCOMPARE(ra[x * 4 + 1], rb[x * 4 + 1]);
            QCOMPARE(ra[x * 4 + 2], rb[x * 4 + 2]);
        }
    }
}

} // namespace

class LosslessCodecTest : public QObject
{
    Q_OBJECT

private slots:
    void ycocgrExhaustiveRoundtrip()
    {
        for (int r = 0; r < 256; ++r) {
            for (int g = 0; g < 256; ++g) {
                for (int b = 0; b < 256; ++b) {
                    int32_t y = 0, co = 0, cg = 0;
                    sds::lossless::rgb8_to_ycocgr(
                        static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b),
                        y, co, cg);
                    uint8_t rr = 0, gg = 0, bb = 0;
                    sds::lossless::ycocgr_to_rgb8(y, co, cg, rr, gg, bb);
                    if (rr != r || gg != g || bb != b) {
                        QFAIL(qPrintable(QString("Mismatch at (%1,%2,%3)").arg(r).arg(g).arg(b)));
                    }
                }
            }
        }
    }

    void crc32cVectorsAndCombine()
    {
        const char* checkStr = "123456789";
        const uint32_t crc = sds::lossless::crc32c_update(
            0, reinterpret_cast<const uint8_t*>(checkStr), 9);
        QCOMPARE(crc, 0xE3069283u);

        // Verify crc32c_combine across odd 1920x1080 and 1366x768 RGB24 frames
        const int w = 1366, h = 768;
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4u);
        uint32_t state = 0x12345678u;
        for (size_t i = 0; i < bgra.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            bgra[i] = static_cast<uint8_t>(state >> 24);
        }

        const uint32_t full = sds::lossless::crc32c_rgb24_from_bgra(bgra.data(), w * 4, w, h);
        uint32_t combined = sds::lossless::crc32c_rgb24_from_bgra_row(bgra.data(), w);
        for (int y = 1; y < h; ++y) {
            const uint32_t rowCrc = sds::lossless::crc32c_rgb24_from_bgra_row(
                bgra.data() + static_cast<size_t>(y) * w * 4u, w);
            combined = sds::lossless::crc32c_combine(combined, rowCrc, static_cast<uint64_t>(w) * 3ULL);
        }
        QCOMPARE(combined, full);
    }

    void blake3KnownVectors()
    {
        // Official BLAKE3 test vectors: empty and 3-byte "abc"
        const auto empty = sds::lossless::blake3_hash(nullptr, 0);
        const QByteArray emptyHex = QByteArray(reinterpret_cast<const char*>(empty.data()), 32).toHex();
        QCOMPARE(emptyHex, QByteArray("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"));

        const uint8_t abc[3] = {'a', 'b', 'c'};
        const auto abcHash = sds::lossless::blake3_hash(abc, 3);
        const QByteArray abcHex = QByteArray(reinterpret_cast<const char*>(abcHash.data()), 32).toHex();
        QCOMPARE(abcHex, QByteArray("6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"));

        // Multi-chunk (> 2 KiB) streaming consistency check
        std::vector<uint8_t> seq(4099);
        for (size_t i = 0; i < seq.size(); ++i) {
            seq[i] = static_cast<uint8_t>(i % 251);
        }
        const auto h1 = sds::lossless::blake3_hash(seq.data(), seq.size());
        const auto h2 = sds::lossless::blake3_hash(seq.data(), seq.size());
        QCOMPARE(h1, h2);
        QVERIFY(h1 != empty);
    }

    void oddResolutionsAndAllTileModesRoundtrip()
    {
        const std::vector<std::pair<int, int>> dims = {
            {1, 1}, {31, 24}, {65, 33}, {1366, 768}, {1920, 1080}
        };

        for (auto [w, h] : dims) {
            std::vector<uint8_t> f1 = makeSolidBgra(w, h, 24, 32, 48, 0x11);
            // Inject diverse content: 2-color text-like, gradient, high-entropy noise
            if (w >= 64 && h >= 32) {
                for (int y = 0; y < 32; ++y) {
                    for (int x = 0; x < 32; ++x) {
                        const bool on = ((x + y * 3) % 5) == 0;
                        fillRectBgra(f1, w, x, y, 1, 1, on ? 250 : 10, on ? 240 : 15, on ? 230 : 20);
                    }
                }
            }
            if (w >= 96 && h >= 32) {
                uint32_t rng = 0xCAFEBABEu;
                for (int y = 0; y < 32; ++y) {
                    for (int x = 64; x < 96; ++x) {
                        rng = rng * 1664525u + 1013904223u;
                        fillRectBgra(f1, w, x, y, 1, 1,
                                     static_cast<uint8_t>(rng >> 24),
                                     static_cast<uint8_t>(rng >> 16),
                                     static_cast<uint8_t>(rng >> 8));
                    }
                }
            }

            sds::lossless::cpu_encoder_t enc(2);
            sds::lossless::cpu_decoder_t dec(2);
            std::vector<uint8_t> pkt;
            sds::lossless::encode_stats_t estats;
            sds::lossless::decode_stats_t dstats;
            sds::lossless::frame_header_t hdr;

            QVERIFY(enc.encode_frame(f1.data(), w * 4, w, h, 1, 1000,
                                     sds::lossless::hash_algo_e::crc32c, true, pkt, &estats));
            QVERIFY(dec.decode_frame(pkt.data(), pkt.size(), true, &hdr, &dstats));
            QVERIFY(dstats.hash_verified);
            QVERIFY(dstats.hash_matched);
            assertRgb24Equal(dec.current_bgra(), dec.stride(), f1.data(), w * 4, w, h);

            // Second frame: identical RGB with different alpha garbage -> 0 dirty tiles!
            std::vector<uint8_t> f2 = f1;
            for (size_t i = 3; i < f2.size(); i += 4) {
                f2[i] ^= 0x7Fu;
            }
            QVERIFY(enc.encode_frame(f2.data(), w * 4, w, h, 2, 2000,
                                     sds::lossless::hash_algo_e::blake3, false, pkt, &estats));
            QCOMPARE(estats.dirty_tiles, 0u);
            QVERIFY(dec.decode_frame(pkt.data(), pkt.size(), true, &hdr, &dstats));
            QVERIFY(dstats.hash_verified);
            QVERIFY(dstats.hash_matched);
            assertRgb24Equal(dec.current_bgra(), dec.stride(), f2.data(), w * 4, w, h);
        }
    }

    void scrollCopyRectUpAndDownRoundtrip()
    {
        const int w = 640, h = 480;
        std::vector<uint8_t> f1(static_cast<size_t>(w) * h * 4u);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
                f1[idx + 0] = static_cast<uint8_t>((x * 3 + y * 7) & 0xFF);
                f1[idx + 1] = static_cast<uint8_t>((y * 5 + (x >> 2)) & 0xFF);
                f1[idx + 2] = static_cast<uint8_t>((x ^ (y * 11)) & 0xFF);
                f1[idx + 3] = 255;
            }
        }

        // Scroll up by 48 pixels (content moves from y+48 to y)
        std::vector<uint8_t> f2 = f1;
        for (int y = 0; y < h - 48; ++y) {
            std::memcpy(f2.data() + static_cast<size_t>(y) * w * 4u,
                        f1.data() + static_cast<size_t>(y + 48) * w * 4u,
                        static_cast<size_t>(w) * 4u);
        }
        fillRectBgra(f2, w, 0, h - 48, w, 48, 12, 34, 56);

        sds::lossless::cpu_encoder_t enc(2);
        sds::lossless::cpu_decoder_t dec(2);
        std::vector<uint8_t> pkt;
        sds::lossless::encode_stats_t estats;
        sds::lossless::decode_stats_t dstats;

        QVERIFY(enc.encode_frame(f1.data(), w * 4, w, h, 1, 100,
                                 sds::lossless::hash_algo_e::crc32c, true, pkt, &estats));
        QVERIFY(dec.decode_frame(pkt.data(), pkt.size(), true, nullptr, &dstats));
        QVERIFY(dstats.hash_matched);

        QVERIFY(enc.encode_frame(f2.data(), w * 4, w, h, 2, 200,
                                 sds::lossless::hash_algo_e::crc32c, false, pkt, &estats));
        QVERIFY(estats.copy_rects >= 1u);
        QVERIFY(estats.dirty_tiles < estats.total_tiles / 2u);
        QVERIFY(dec.decode_frame(pkt.data(), pkt.size(), true, nullptr, &dstats));
        QVERIFY(dstats.hash_matched);
        assertRgb24Equal(dec.current_bgra(), dec.stride(), f2.data(), w * 4, w, h);

        // Scroll down by 64 pixels
        std::vector<uint8_t> f3 = f2;
        for (int y = h - 1; y >= 64; --y) {
            std::memcpy(f3.data() + static_cast<size_t>(y) * w * 4u,
                        f2.data() + static_cast<size_t>(y - 64) * w * 4u,
                        static_cast<size_t>(w) * 4u);
        }
        fillRectBgra(f3, w, 0, 0, w, 64, 99, 88, 77);

        QVERIFY(enc.encode_frame(f3.data(), w * 4, w, h, 3, 300,
                                 sds::lossless::hash_algo_e::blake3, false, pkt, &estats));
        QVERIFY(estats.copy_rects >= 1u);
        QVERIFY(dec.decode_frame(pkt.data(), pkt.size(), true, nullptr, &dstats));
        QVERIFY(dstats.hash_matched);
        assertRgb24Equal(dec.current_bgra(), dec.stride(), f3.data(), w * 4, w, h);
    }

    void malformedPacketsFailCleanly()
    {
        sds::lossless::cpu_encoder_t enc(1);
        sds::lossless::cpu_decoder_t dec(1);
        std::vector<uint8_t> f = makeSolidBgra(64, 64, 1, 2, 3);
        std::vector<uint8_t> pkt;
        QVERIFY(enc.encode_frame(f.data(), 64 * 4, 64, 64, 1, 0,
                                 sds::lossless::hash_algo_e::crc32c, true, pkt));

        // Truncated packet
        QVERIFY(!dec.decode_frame(pkt.data(), pkt.size() - 1, true));
        QVERIFY(!dec.decode_frame(pkt.data(), 10, true));

        // Bad magic
        std::vector<uint8_t> bad = pkt;
        bad[0] = 'X';
        QVERIFY(!dec.decode_frame(bad.data(), bad.size(), true));

        // Delta frame before keyframe must fail
        std::vector<uint8_t> deltaPkt;
        f[0] ^= 1;
        QVERIFY(enc.encode_frame(f.data(), 64 * 4, 64, 64, 2, 0,
                                 sds::lossless::hash_algo_e::crc32c, false, deltaPkt));
        sds::lossless::cpu_decoder_t freshDec(1);
        QVERIFY(!freshDec.decode_frame(deltaPkt.data(), deltaPkt.size(), true));

        // Corrupt payload hash triggers hash_matched == false
        std::vector<uint8_t> corruptedHash = pkt;
        corruptedHash[40] ^= 0x01;
        sds::lossless::decode_stats_t dstats;
        QVERIFY(freshDec.decode_frame(corruptedHash.data(), corruptedHash.size(), true, nullptr, &dstats));
        QVERIFY(dstats.hash_verified);
        QVERIFY(!dstats.hash_matched);
    }

    void clientLoopbackUpgradeAndStreamRoundtrip()
    {
        const TestCert hostCert = TestCerts::makeSelfSigned("host-lossless");
        const TestCert clientCert = TestCerts::makeSelfSigned("client-lossless");

        QSslConfiguration srvCfg = QSslConfiguration::defaultConfiguration();
        srvCfg.setLocalCertificate(hostCert.cert());
        srvCfg.setPrivateKey(hostCert.key());
        srvCfg.setCaCertificates({clientCert.cert()});
        srvCfg.setPeerVerifyMode(QSslSocket::VerifyPeer);

        QSslServer server;
        server.setSslConfiguration(srvCfg);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        const int w = 128, h = 96;
        std::vector<uint8_t> f1 = makeSolidBgra(w, h, 30, 140, 220);
        std::vector<uint8_t> f2 = f1;
        fillRectBgra(f2, w, 16, 16, 48, 32, 240, 50, 20);

        sds::lossless::cpu_encoder_t enc(1);
        std::vector<uint8_t> pkt1, pkt2;
        QVERIFY(enc.encode_frame(f1.data(), w * 4, w, h, 1, 1000,
                                 sds::lossless::hash_algo_e::crc32c, true, pkt1));
        QVERIFY(enc.encode_frame(f2.data(), w * 4, w, h, 2, 2000,
                                 sds::lossless::hash_algo_e::crc32c, false, pkt2));

        bool sawUpgrade = false;
        bool sawKeyframeReq = false;
        QByteArray rxBuf;

        QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&]() {
            while (QTcpSocket* sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, &server, [&, sock]() {
                    rxBuf.append(sock->readAll());
                    if (!sawUpgrade) {
                        const int end = rxBuf.indexOf("\r\n\r\n");
                        if (end >= 0) {
                            const QByteArray req = rxBuf.left(end);
                            rxBuf.remove(0, end + 4);
                            QVERIFY(req.contains("GET /sds/v1/lossless-stream HTTP/1.1"));
                            QVERIFY(req.toLower().contains("upgrade: sds-lossless-v1"));
                            sawUpgrade = true;
                            sock->write("HTTP/1.1 101 Switching Protocols\r\n"
                                        "Upgrade: sds-lossless-v1\r\n"
                                        "Connection: Upgrade\r\n\r\n");
                            sock->write(reinterpret_cast<const char*>(pkt1.data()), static_cast<qint64>(pkt1.size()));
                            sock->write(reinterpret_cast<const char*>(pkt2.data()), static_cast<qint64>(pkt2.size()));
                            sock->flush();
                        }
                    }
                    while (sawUpgrade && rxBuf.size() >= static_cast<int>(sds::lossless::CONTROL_MSG_SIZE)) {
                        sds::lossless::control_msg_t msg {};
                        if (sds::lossless::parse_control_msg(
                                reinterpret_cast<const uint8_t*>(rxBuf.constData()),
                                static_cast<size_t>(rxBuf.size()), msg)) {
                            if (msg.type == sds::lossless::control_msg_e::request_keyframe) {
                                sawKeyframeReq = true;
                            }
                        }
                        rxBuf.remove(0, static_cast<int>(sds::lossless::CONTROL_MSG_SIZE));
                    }
                });
            }
        });

        SdsLosslessClient::Config cfg;
        cfg.host = QStringLiteral("127.0.0.1");
        cfg.httpsPort = server.serverPort();
        cfg.serverCert = hostCert.cert();
        cfg.clientCert = clientCert.cert();
        cfg.clientKey = clientCert.key();
        cfg.hashAlgo = sds::lossless::hash_algo_e::crc32c;

        SdsLosslessClient client(cfg);
        client.start();

        SdsLosslessClient::FramePacket out1, out2;
        QElapsedTimer timer;
        timer.start();
        bool got1 = false, got2 = false;
        while (timer.elapsed() < 5000 && !(got1 && got2 && sawKeyframeReq)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (!got1 && client.popFrame(out1, 10)) {
                got1 = true;
            }
            if (got1 && !got2 && client.popFrame(out2, 10)) {
                got2 = true;
            }
        }
        client.stop();

        QVERIFY(sawUpgrade);
        QVERIFY(sawKeyframeReq);
        QVERIFY(got1);
        QVERIFY(got2);

        sds::lossless::cpu_decoder_t dec(1);
        sds::lossless::decode_stats_t dstats;
        QVERIFY(dec.decode_frame(reinterpret_cast<const uint8_t*>(out1.bytes.constData()),
                                 static_cast<size_t>(out1.bytes.size()), true, nullptr, &dstats));
        QVERIFY(dstats.hash_matched);
        assertRgb24Equal(dec.current_bgra(), dec.stride(), f1.data(), w * 4, w, h);

        QVERIFY(dec.decode_frame(reinterpret_cast<const uint8_t*>(out2.bytes.constData()),
                                 static_cast<size_t>(out2.bytes.size()), true, nullptr, &dstats));
        QVERIFY(dstats.hash_matched);
        assertRgb24Equal(dec.current_bgra(), dec.stride(), f2.data(), w * 4, w, h);
    }

    void clientRejectsWrongPin()
    {
        const TestCert realHost = TestCerts::makeSelfSigned("real-host");
        const TestCert wrongPin = TestCerts::makeSelfSigned("wrong-host");
        const TestCert clientCert = TestCerts::makeSelfSigned("client-lossless");

        QSslConfiguration srvCfg = QSslConfiguration::defaultConfiguration();
        srvCfg.setLocalCertificate(realHost.cert());
        srvCfg.setPrivateKey(realHost.key());
        srvCfg.setPeerVerifyMode(QSslSocket::VerifyNone);

        QSslServer server;
        server.setSslConfiguration(srvCfg);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        SdsLosslessClient::Config cfg;
        cfg.host = QStringLiteral("127.0.0.1");
        cfg.httpsPort = server.serverPort();
        cfg.serverCert = wrongPin.cert();
        cfg.clientCert = clientCert.cert();
        cfg.clientKey = clientCert.key();

        SdsLosslessClient client(cfg);
        client.start();

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000 && !client.hadPinFailure()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        client.stop();

        QVERIFY(client.hadPinFailure());
        QVERIFY(!client.isConnected());
    }

    void malformedKeyframeAndHashTiming()
    {
        const int w = 128, h = 96;
        std::vector<uint8_t> f1 = makeSolidBgra(w, h, 20, 40, 60);
        std::vector<uint8_t> f2 = f1;
        fillRectBgra(f2, w, 0, 0, 32, 32, 200, 100, 50);

        sds::lossless::cpu_encoder_t enc(1);
        sds::lossless::cpu_decoder_t dec(1);
        std::vector<uint8_t> pkt1, pkt2;
        sds::lossless::decode_stats_t dstats {};

        QVERIFY(enc.encode_frame(f1.data(), w * 4, w, h, 1, 1000,
                                 sds::lossless::hash_algo_e::crc32c, true, pkt1));
        QVERIFY(dec.decode_frame(pkt1.data(), pkt1.size(), true, nullptr, &dstats));
        QVERIFY(dstats.hash_verified);
        QVERIFY(dstats.hash_matched);
        QVERIFY(dstats.hash_us < 1000000u);

        QVERIFY(enc.encode_frame(f2.data(), w * 4, w, h, 2, 2000,
                                 sds::lossless::hash_algo_e::blake3, false, pkt2));
        // Mutate delta packet flags (byte offset 6) to claim FLAG_KEYFRAME -> decoder must reject incomplete tile coverage
        std::vector<uint8_t> bad = pkt2;
        bad[6] |= static_cast<uint8_t>(sds::lossless::FLAG_KEYFRAME & 0xFFu);
        QVERIFY(!dec.decode_frame(bad.data(), bad.size(), false, nullptr, nullptr));
    }

    void blake3MultiChunkBoundaries()
    {
        for (std::size_t pixels : {std::size_t {341}, std::size_t {342}, std::size_t {683}, std::size_t {1025}, std::size_t {3414}}) {
            const int w = static_cast<int>(pixels);
            std::vector<std::uint8_t> rgb(pixels * 3u);
            std::vector<std::uint8_t> bgra(pixels * 4u);
            for (std::size_t i = 0; i < pixels; ++i) {
                const std::uint8_t r = static_cast<std::uint8_t>((i * 41u + 7u) & 0xFFu);
                const std::uint8_t g = static_cast<std::uint8_t>((i * 73u + 19u) & 0xFFu);
                const std::uint8_t b = static_cast<std::uint8_t>((i * 19u + 93u) & 0xFFu);
                rgb[i * 3u + 0] = r;
                rgb[i * 3u + 1] = g;
                rgb[i * 3u + 2] = b;
                bgra[i * 4u + 0] = b;
                bgra[i * 4u + 1] = g;
                bgra[i * 4u + 2] = r;
                bgra[i * 4u + 3] = static_cast<std::uint8_t>(i & 0xFFu);
            }
            const auto oneShot = sds::lossless::blake3_hash(rgb.data(), rgb.size());
            const auto fromBgra = sds::lossless::blake3_rgb24_from_bgra(bgra.data(), w * 4, w, 1);
            QCOMPARE(fromBgra, oneShot);

            bgra[pixels * 4u - 2u] ^= 0x01u;
            QVERIFY(sds::lossless::blake3_rgb24_from_bgra(bgra.data(), w * 4, w, 1) != oneShot);
        }
    }

    void clientRejects101WithoutConnectionUpgrade()
    {
        const TestCert hostCert = TestCerts::makeSelfSigned("host-no-conn");
        const TestCert clientCert = TestCerts::makeSelfSigned("cli-no-conn");

        QSslConfiguration srvCfg = QSslConfiguration::defaultConfiguration();
        srvCfg.setLocalCertificate(hostCert.cert());
        srvCfg.setPrivateKey(hostCert.key());
        srvCfg.setCaCertificates({clientCert.cert()});
        srvCfg.setPeerVerifyMode(QSslSocket::VerifyPeer);

        QSslServer server;
        server.setSslConfiguration(srvCfg);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        QByteArray rxBuf;
        QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&]() {
            while (QTcpSocket* sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, &server, [&, sock]() {
                    rxBuf.append(sock->readAll());
                    if (rxBuf.contains("\r\n\r\n")) {
                        sock->write("HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: sds-lossless-v1\r\n\r\n");
                        sock->flush();
                        rxBuf.clear();
                    }
                });
            }
        });

        SdsLosslessClient::Config cfg;
        cfg.host = QStringLiteral("127.0.0.1");
        cfg.httpsPort = server.serverPort();
        cfg.serverCert = hostCert.cert();
        cfg.clientCert = clientCert.cert();
        cfg.clientKey = clientCert.key();

        SdsLosslessClient client(cfg);
        client.start();

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 2000 && client.lastError().isEmpty()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        client.stop();

        QVERIFY(!client.isConnected());
        QVERIFY(!client.lastError().isEmpty());
    }
};

int runLosslessCodecTests(int argc, char** argv)
{
    LosslessCodecTest t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_lossless_codec.moc"
