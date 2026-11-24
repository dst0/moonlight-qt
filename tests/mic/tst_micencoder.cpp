// Opus encoder framing and settings (DESIGN.md 7.2: 48 kHz mono, 20 ms, VOIP, 48 kbit/s VBR, FEC, no DTX)
#include "testutil.h"
#include "streaming/audio/micstreamer.h"

#include <QtTest>
#include <opus.h>

class TestMicEncoder : public QObject
{
    Q_OBJECT

private slots:
    void settingsMatchDesign()
    {
        MicOpusEncoder encoder;
        QCOMPARE(encoder.queryBitrate(), -1);  // not initialized
        QVERIFY(encoder.init());
        QVERIFY(encoder.init());  // idempotent
        QCOMPARE(encoder.queryApplication(), OPUS_APPLICATION_VOIP);
        QCOMPARE(encoder.queryBitrate(), MicOpusEncoder::kBitrate);
        QCOMPARE(encoder.queryVbr(), 1);
        QCOMPARE(encoder.queryInbandFec(), 1);
        QCOMPARE(encoder.queryDtx(), 0);
    }

    void emitsOnePacketPer20msFrame()
    {
        MicOpusEncoder encoder;
        QVERIFY(encoder.init());
        SineGenerator sine;
        std::vector<std::vector<unsigned char>> packets;
        auto sink = [&](const unsigned char* data, int length) {
            packets.emplace_back(data, data + length);
        };

        auto samples = sine.next(MicOpusEncoder::kFrameSamples * 3 + 100);
        QCOMPARE(encoder.pushSamples(samples.data(), (int)samples.size(), sink), 3);
        QCOMPARE(encoder.pendingSamples(), 100);

        // Odd chunk sizes (CoreAudio delivers e.g. 441 or 512 frames) still produce whole frames
        int emitted = 0;
        for (int i = 0; i < 40; i++) {
            auto chunk = sine.next(441);
            emitted += encoder.pushSamples(chunk.data(), (int)chunk.size(), sink);
        }
        QCOMPARE(emitted, (100 + 40 * 441) / MicOpusEncoder::kFrameSamples);
        QCOMPARE(encoder.pendingSamples(), (100 + 40 * 441) % MicOpusEncoder::kFrameSamples);

        for (const auto& packet : packets) {
            QVERIFY(packet.size() > 0);
            QVERIFY((int)packet.size() <= MicOpusEncoder::kMaxPacketBytes);
            QCOMPARE(opus_packet_get_nb_samples(packet.data(), (opus_int32)packet.size(), 48000), MicOpusEncoder::kFrameSamples);
            QCOMPARE(opus_packet_get_nb_channels(packet.data()), 1);
            QCOMPARE((int)decodeOpus(packet.data(), (int)packet.size()).size(), MicOpusEncoder::kFrameSamples);
        }

        encoder.reset();
        QCOMPARE(encoder.pendingSamples(), 0);
    }

    void bitrateIsRoughly48kbps()
    {
        MicOpusEncoder encoder;
        QVERIFY(encoder.init());
        SineGenerator sine(300.0, 0.3);
        size_t bytes = 0;
        auto samples = sine.next(48000 * 2);  // 2 s
        int packets = encoder.pushSamples(samples.data(), (int)samples.size(), [&](const unsigned char*, int length) {
            bytes += length;
        });
        QCOMPARE(packets, 100);
        double kbps = bytes * 8.0 / 2.0 / 1000.0;
        QVERIFY2(kbps > 20 && kbps < 70, qPrintable(QString("bitrate %1 kbps").arg(kbps)));
    }

    void rejectsBadInputAndUninitializedUse()
    {
        MicOpusEncoder encoder;
        float sample = 0;
        QCOMPARE(encoder.pushSamples(&sample, 1, nullptr), -1);
        QVERIFY(encoder.init());
        QCOMPARE(encoder.pushSamples(nullptr, 5, nullptr), -1);
        QCOMPARE(encoder.pushSamples(&sample, -1, nullptr), -1);
        QCOMPARE(encoder.pushSamples(nullptr, 0, nullptr), 0);
    }
};

MIC_REGISTER_TEST(TestMicEncoder);
#include "tst_micencoder.moc"
