// End to end on the client: fake capture -> MicStreamer -> LiSendMicrophoneOpusData() (real
// moonlight-common-c mic stream) -> loopback UDP. The datagrams are decrypted independently
// with OpenSSL (header as AAD, IV = seq32le || 0^6 || 'C' 'M') and decoded with libopus.
#include "testutil.h"
#include "streaming/audio/micstreamer.h"

#include <Limelight.h>

#include <QtTest>
#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" int micTestStartStream(uint16_t port, const char pingPayload[16], const unsigned char key[16]);
extern "C" void micTestStopStream(void);

namespace {

const unsigned char kKey[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
const char kPing[16] = { 'm','i','c','-','w','i','r','e','-','t','e','s','t','-','0','1' };

uint32_t be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

std::vector<unsigned char> decrypt(const std::vector<unsigned char>& d)
{
    if (d.size() < 29) {
        return {};
    }
    unsigned char iv[12] = { d[7], d[6], d[5], d[4], 0, 0, 0, 0, 0, 0, 'C', 'M' };
    std::vector<unsigned char> plain(d.size() - 28);
    int len = 0, fin = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, kKey, iv) == 1 &&
              EVP_DecryptUpdate(ctx, nullptr, &len, d.data(), 12) == 1 &&
              EVP_DecryptUpdate(ctx, plain.data(), &len, d.data() + 28, (int)d.size() - 28) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)(d.data() + 12)) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain.data() + len, &fin) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return {};
    }
    plain.resize(len + fin);
    return plain;
}

}

class TestMicWire : public QObject
{
    Q_OBJECT

private slots:
    void capturedAudioArrivesEncryptedAndDecodable()
    {
        int rx = socket(AF_INET, SOCK_DGRAM, 0);
        QVERIFY(rx >= 0);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t addrLen = sizeof(addr);
        QCOMPARE(bind(rx, (sockaddr*)&addr, sizeof(addr)), 0);
        QCOMPARE(getsockname(rx, (sockaddr*)&addr, &addrLen), 0);

        QVERIFY(!LiIsMicrophoneStreamActive());
        QCOMPARE(micTestStartStream(ntohs(addr.sin_port), kPing, kKey), 0);
        QVERIFY(LiIsMicrophoneStreamActive());

        auto source = std::make_unique<FakeCaptureSource>();
        FakeCaptureSource* fake = source.get();
        MicStreamer streamer(std::move(source));  // default sender: LiSendMicrophoneOpusData
        QVERIFY(streamer.start());

        SineGenerator sine(440.0, 0.25);
        for (int i = 0; i < 25; i++) {  // 0.5 s in 20 ms chunks
            auto chunk = sine.next(960);
            fake->deliver(chunk.data(), (int)chunk.size());
            QTest::qWait(2);
        }
        QTRY_COMPARE(streamer.framesSent(), (uint64_t)25);

        std::vector<float> decoded;
        StatefulOpusDecoder decoder;
        uint32_t expectedSeq = 0;
        int64_t previousTimestamp = -960;
        int pings = 0;
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(rx, &fds);
            timeval tv = { 0, 300000 };
            if (select(rx + 1, &fds, nullptr, nullptr, &tv) <= 0) {
                break;
            }
            std::vector<unsigned char> d(2048);
            ssize_t n = recv(rx, d.data(), d.size(), 0);
            QVERIFY(n > 0);
            d.resize(n);
            if (n == 20) {
                QVERIFY(memcmp(d.data(), kPing, 16) == 0);
                pings++;
                continue;
            }
            QCOMPARE((int)d[0], 1);
            QCOMPARE(be32(&d[4]), expectedSeq);
            // 960 samples per frame; larger jumps only after a >100 ms scheduling gap
            QCOMPARE(be32(&d[8]) % 960, 0u);
            QVERIFY((int64_t)be32(&d[8]) >= previousTimestamp + 960);
            previousTimestamp = be32(&d[8]);
            QVERIFY(n <= 1400);
            auto opus = decrypt(d);
            QVERIFY2(!opus.empty(), "GCM authentication failed");
            auto pcm = decoder.decode(opus.data(), (int)opus.size());
            QCOMPARE((int)pcm.size(), 960);
            decoded.insert(decoded.end(), pcm.begin(), pcm.end());
            expectedSeq++;
        }
        QCOMPARE(expectedSeq, 25u);
        QVERIFY(pings >= 1);

        // After the codec settles, the tone is there (RMS of a 0.25 sine is ~0.177)
        double level = rms(decoded, 960 * 5);
        QVERIFY2(level > 0.09 && level < 0.3, qPrintable(QString("rms %1").arg(level)));

        streamer.stop();
        micTestStopStream();
        QVERIFY(!LiIsMicrophoneStreamActive());
        unsigned char frame[3] = { 0xf8, 0xff, 0xfe };
        QCOMPARE(LiSendMicrophoneOpusData(frame, sizeof(frame)), LI_ERR_UNSUPPORTED);
        close(rx);
    }
};

MIC_REGISTER_TEST(TestMicWire);
#include "tst_micwire.moc"
