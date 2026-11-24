#include <QtTest>

#include <QSet>

#include "streaming/video/packed444.h"
#include "tests.h"

namespace {

inline int floorDiv2(int v)
{
    return (v >= 0) ? (v >> 1) : -((-v + 1) >> 1);
}

struct Packed10Sample
{
    uint16_t y10;
    uint16_t co10;
    uint16_t cg10;
};

Packed10Sample encodeYCoCgR10(uint8_t r, uint8_t g, uint8_t b)
{
    const int co = static_cast<int>(r) - static_cast<int>(b);
    const int t = static_cast<int>(b) + floorDiv2(co);
    const int cg = static_cast<int>(g) - t;
    const int y = t + floorDiv2(cg);

    return Packed10Sample{
        static_cast<uint16_t>(y * 4),
        static_cast<uint16_t>(512 + co * 2),
        static_cast<uint16_t>(512 + cg * 2),
    };
}

void decodeYCoCgR10(uint16_t y10, uint16_t co10, uint16_t cg10,
                    uint8_t& outR, uint8_t& outG, uint8_t& outB)
{
    const int y = qBound(0, static_cast<int>(std::rint(static_cast<float>(y10) * 0.25f)), 255);
    const int co = qBound(-255, static_cast<int>(std::rint(static_cast<float>(static_cast<int>(co10) - 512) * 0.5f)), 255);
    const int cg = qBound(-255, static_cast<int>(std::rint(static_cast<float>(static_cast<int>(cg10) - 512) * 0.5f)), 255);

    const int t = y - floorDiv2(cg);
    const int g = qBound(0, cg + t, 255);
    const int b = qBound(0, t - floorDiv2(co), 255);
    const int r = qBound(0, b + co, 255);

    outR = static_cast<uint8_t>(r);
    outG = static_cast<uint8_t>(g);
    outB = static_cast<uint8_t>(b);
}

Packed444Inputs makeValidPackedInputs()
{
    Packed444Inputs in;
    in.enableYUV444 = true;
    in.enableHdr = false;
    in.videoCodecConfig = StreamingPreferences::VCC_AUTO;
    in.videoDecoderSelection = StreamingPreferences::VDS_AUTO;
    in.rendererSelection = StreamingPreferences::RS_AUTO;
    in.hostEncoderIsSoftware = false;
    in.hostSupportsHwYCoCgR444 = true;
    in.hostNativeYuv444Supported = false;
    in.maxPackedW = 8192;
    in.maxPackedH = 4352;
    in.logicalW = 1920;
    in.logicalH = 1080;
    in.isMacOSMetalAvailable = true;
    return in;
}

} // namespace

class TestPacked444 : public QObject
{
    Q_OBJECT

private slots:
    void layoutGeometry_data()
    {
        QTest::addColumn<int>("logicalW");
        QTest::addColumn<int>("logicalH");
        QTest::addColumn<int>("maxEncW");
        QTest::addColumn<int>("maxEncH");
        QTest::addColumn<int>("expTileW");
        QTest::addColumn<int>("expSplitX");
        QTest::addColumn<int>("expHalfW");
        QTest::addColumn<int>("expTileH");
        QTest::addColumn<int>("expEncW");
        QTest::addColumn<int>("expEncH");
        QTest::addColumn<bool>("expValid");

        QTest::newRow("1080p") << 1920 << 1080 << 8192 << 4352 << 1920 << 960 << 960 << 1088 << 2880 << 2176 << true;
        QTest::newRow("1440p") << 2560 << 1440 << 8192 << 4352 << 2560 << 1280 << 1280 << 1472 << 3840 << 2944 << true;
        QTest::newRow("2880x1864") << 2880 << 1864 << 8192 << 4352 << 2880 << 1440 << 1472 << 1920 << 4352 << 3840 << true;
        QTest::newRow("3024x1964") << 3024 << 1964 << 8192 << 4352 << 3072 << 1512 << 1536 << 1984 << 4608 << 3968 << true;
        QTest::newRow("4K_boundary") << 3840 << 2160 << 8192 << 4352 << 3840 << 1920 << 1920 << 2176 << 5760 << 4352 << true;
        QTest::newRow("odd_1511x981") << 1511 << 981 << 8192 << 4352 << 1536 << 756 << 768 << 1024 << 2304 << 2048 << true;
        QTest::newRow("min_64x64") << 64 << 64 << 8192 << 4352 << 64 << 32 << 64 << 64 << 128 << 128 << true;
        QTest::newRow("oversized_5120x2880") << 5120 << 2880 << 8192 << 4352 << 5120 << 2560 << 2560 << 2880 << 7680 << 5760 << false;
        QTest::newRow("invalid_63x64") << 63 << 64 << 8192 << 4352 << 64 << 32 << 64 << 64 << 128 << 128 << false;
        QTest::newRow("invalid_64x63") << 64 << 63 << 8192 << 4352 << 64 << 32 << 64 << 64 << 128 << 128 << false;
        QTest::newRow("invalid_0x0") << 0 << 0 << 8192 << 4352 << 0 << 0 << 0 << 0 << 0 << 0 << false;
        QTest::newRow("custom_limit_exceeded") << 2880 << 1864 << 4096 << 4096 << 2880 << 1440 << 1472 << 1920 << 4352 << 3840 << false;
    }

    void layoutGeometry()
    {
        QFETCH(int, logicalW);
        QFETCH(int, logicalH);
        QFETCH(int, maxEncW);
        QFETCH(int, maxEncH);
        QFETCH(int, expTileW);
        QFETCH(int, expSplitX);
        QFETCH(int, expHalfW);
        QFETCH(int, expTileH);
        QFETCH(int, expEncW);
        QFETCH(int, expEncH);
        QFETCH(bool, expValid);

        const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(logicalW, logicalH, maxEncW, maxEncH);
        QCOMPARE(layout.logicalW, logicalW);
        QCOMPARE(layout.logicalH, logicalH);
        QCOMPARE(layout.tileW, expTileW);
        QCOMPARE(layout.splitX, expSplitX);
        QCOMPARE(layout.halfW, expHalfW);
        QCOMPARE(layout.tileH, expTileH);
        QCOMPARE(layout.encW, expEncW);
        QCOMPARE(layout.encH, expEncH);
        QCOMPARE(layout.valid, expValid);

        if (layout.valid) {
            QCOMPARE(layout.tileW % 64, 0);
            QCOMPARE(layout.halfW % 64, 0);
            QCOMPARE(layout.tileH % 64, 0);
            QCOMPARE(layout.encW % 64, 0);
            QCOMPARE(layout.encH % 64, 0);
            QVERIFY(layout.logicalW <= layout.tileW);
            QVERIFY(layout.logicalH <= layout.tileH);
            QVERIFY(layout.splitX <= layout.halfW);
            QVERIFY((layout.logicalW - layout.splitX) <= layout.halfW);
        }
    }

    void layoutRegionsNeverOverlapForOddDimensions()
    {
        const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(131, 67);
        QVERIFY(layout.valid);

        QSet<quint64> coords;
        coords.reserve(layout.logicalW * layout.logicalH * 3);
        auto packCoord = [](int x, int y) -> quint64 {
            return (static_cast<quint64>(static_cast<uint32_t>(x)) << 32) |
                   static_cast<uint32_t>(y);
        };

        for (int y = 0; y < layout.logicalH; ++y) {
            for (int x = 0; x < layout.logicalW; ++x) {
                const quint64 cY = packCoord(x, y);
                const quint64 cCo = packCoord(x, layout.tileH + y);
                const quint64 cCg = (x < layout.splitX)
                    ? packCoord(layout.tileW + x, y)
                    : packCoord(layout.tileW + (x - layout.splitX), layout.tileH + y);
                QVERIFY(x < layout.encW && y < layout.encH);
                QVERIFY((layout.tileH + y) < layout.encH);
                const int cgX = (x < layout.splitX) ? (layout.tileW + x) : (layout.tileW + (x - layout.splitX));
                const int cgY = (x < layout.splitX) ? y : (layout.tileH + y);
                QVERIFY(cgX >= layout.tileW && cgX < layout.encW);
                QVERIFY(cgY >= 0 && cgY < layout.encH);
                QVERIFY(!coords.contains(cY));
                coords.insert(cY);
                QVERIFY(!coords.contains(cCo));
                coords.insert(cCo);
                QVERIFY(!coords.contains(cCg));
                coords.insert(cCg);
            }
        }
        QCOMPARE(coords.size(), layout.logicalW * layout.logicalH * 3);
    }

    void decisionEdgeCases_data()
    {
        QTest::addColumn<bool>("enableYUV444");
        QTest::addColumn<bool>("enableHdr");
        QTest::addColumn<int>("vcc");
        QTest::addColumn<int>("vds");
        QTest::addColumn<int>("renderer");
        QTest::addColumn<bool>("hostSw");
        QTest::addColumn<bool>("hostHwPacked");
        QTest::addColumn<bool>("hostNative444");
        QTest::addColumn<int>("logicalW");
        QTest::addColumn<int>("logicalH");
        QTest::addColumn<bool>("metalAvail");
        QTest::addColumn<bool>("expUsePacked");
        QTest::addColumn<bool>("expForceMain10");
        QTest::addColumn<bool>("expKeepBitrate");
        QTest::addColumn<QString>("expWarningSubstring");

        using SP = StreamingPreferences;

        // 1. YUV 4:4:4 disabled
        QTest::newRow("01_disabled")
            << false << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << true << false << 1920 << 1080 << true
            << false << false << false << QString();

        // 2. Host native 4:4:4 supported (e.g. NVENC/QSV), even with HDR/H264/Vulkan
        QTest::newRow("02_host_native_444")
            << true << true << int(SP::VCC_FORCE_H264) << int(SP::VDS_AUTO) << int(SP::RS_VULKAN)
            << false << true << true << 1920 << 1080 << true
            << false << false << false << QString();

        // 3. Host encoder is software
        QTest::newRow("03_host_software_encoder")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << true << true << false << 1920 << 1080 << true
            << false << false << false << QString();

        // 4. Legacy host without hw_ycocgr_444
        QTest::newRow("04_legacy_host_unsupported")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << false << false << 1920 << 1080 << true
            << false << false << false << QString();

        // 5. HDR enabled -> disallowed with warning
        QTest::newRow("05_hdr_conflict")
            << true << true << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << true << false << 1920 << 1080 << true
            << false << false << false
            << QStringLiteral("Hardware packed 4:4:4 cannot be combined with HDR; streaming 10-bit HDR 4:2:0.");

        // 6. Force H.264 -> disallowed with warning
        QTest::newRow("06_force_h264")
            << true << false << int(SP::VCC_FORCE_H264) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << true << false << 1920 << 1080 << true
            << false << false << false
            << QStringLiteral("Hardware packed 4:4:4 requires Automatic or HEVC codec.");

        // 7. Force AV1 -> disallowed with warning
        QTest::newRow("07_force_av1")
            << true << false << int(SP::VCC_FORCE_AV1) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << true << false << 1920 << 1080 << true
            << false << false << false
            << QStringLiteral("Hardware packed 4:4:4 requires Automatic or HEVC codec.");

        // 8. Force Vulkan renderer -> disallowed with warning
        QTest::newRow("08_force_vulkan")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_VULKAN)
            << false << true << false << 1920 << 1080 << true
            << false << false << false
            << QStringLiteral("Hardware packed 4:4:4 requires Automatic or Metal renderer.");

        // 9. Force AVSBDL renderer -> disallowed with warning
        QTest::newRow("09_force_avsbdl")
            << true << false << int(SP::VCC_FORCE_HEVC) << int(SP::VDS_AUTO) << int(SP::RS_AVSBDL)
            << false << true << false << 1920 << 1080 << true
            << false << false << false
            << QStringLiteral("Hardware packed 4:4:4 requires Automatic or Metal renderer.");

        // 10. Non-macOS / Metal unavailable -> disallowed with warning
        QTest::newRow("10_metal_unavailable")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << true << false << 1920 << 1080 << false
            << false << false << false
            << QStringLiteral("Hardware packed 4:4:4 requires Automatic or Metal renderer.");

        // 11. Oversized resolution 5120x2880 -> disallowed with max 3840x2160 warning
        QTest::newRow("11_oversized_5k")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_AUTO)
            << false << true << false << 5120 << 2880 << true
            << false << false << false
            << QStringLiteral("3840x2160");

        // 12. Undersized resolution < 64 -> disallowed with warning
        QTest::newRow("12_undersized_32x32")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_AUTO) << int(SP::RS_METAL)
            << false << true << false << 32 << 32 << true
            << false << false << false
            << QStringLiteral("64x64");

        // 13. VDS_FORCE_SOFTWARE + Auto/Metal -> ALLOWED!
        QTest::newRow("13_sw_decoder_auto_renderer_allowed")
            << true << false << int(SP::VCC_AUTO) << int(SP::VDS_FORCE_SOFTWARE) << int(SP::RS_AUTO)
            << false << true << false << 1920 << 1080 << true
            << true << true << true << QString();

        // 14. Valid Auto/HEVC + Metal + VDS_FORCE_HARDWARE + 4K boundary
        QTest::newRow("14_valid_hevc_metal_hw_4k")
            << true << false << int(SP::VCC_FORCE_HEVC) << int(SP::VDS_FORCE_HARDWARE) << int(SP::RS_METAL)
            << false << true << false << 3840 << 2160 << true
            << true << true << true << QString();

        // 15. Deprecated HEVC HDR codec enum + Auto renderer + VDS_FORCE_SOFTWARE -> ALLOWED
        QTest::newRow("15_valid_hevc_deprecated_sw_metal")
            << true << false << int(SP::VCC_FORCE_HEVC_HDR_DEPRECATED) << int(SP::VDS_FORCE_SOFTWARE) << int(SP::RS_METAL)
            << false << true << false << 2880 << 1864 << true
            << true << true << true << QString();
    }

    void decisionEdgeCases()
    {
        QFETCH(bool, enableYUV444);
        QFETCH(bool, enableHdr);
        QFETCH(int, vcc);
        QFETCH(int, vds);
        QFETCH(int, renderer);
        QFETCH(bool, hostSw);
        QFETCH(bool, hostHwPacked);
        QFETCH(bool, hostNative444);
        QFETCH(int, logicalW);
        QFETCH(int, logicalH);
        QFETCH(bool, metalAvail);
        QFETCH(bool, expUsePacked);
        QFETCH(bool, expForceMain10);
        QFETCH(bool, expKeepBitrate);
        QFETCH(QString, expWarningSubstring);

        Packed444Inputs in = makeValidPackedInputs();
        in.enableYUV444 = enableYUV444;
        in.enableHdr = enableHdr;
        in.videoCodecConfig = static_cast<StreamingPreferences::VideoCodecConfig>(vcc);
        in.videoDecoderSelection = static_cast<StreamingPreferences::VideoDecoderSelection>(vds);
        in.rendererSelection = static_cast<StreamingPreferences::RendererSelection>(renderer);
        in.hostEncoderIsSoftware = hostSw;
        in.hostSupportsHwYCoCgR444 = hostHwPacked;
        in.hostNativeYuv444Supported = hostNative444;
        in.logicalW = logicalW;
        in.logicalH = logicalH;
        in.isMacOSMetalAvailable = metalAvail;

        const Packed444Decision d = resolvePacked444Decision(in);
        QCOMPARE(d.usePackedYCoCgRA1, expUsePacked);
        QCOMPARE(d.forceHevcMain10, expForceMain10);
        QCOMPARE(d.keep444Bitrate, expKeepBitrate);
        if (expWarningSubstring.isEmpty()) {
            QCOMPARE(d.userWarning, QString());
        }
        else {
            QVERIFY2(d.userWarning.contains(expWarningSubstring),
                     qPrintable(QStringLiteral("Actual warning: ") + d.userWarning));
        }
    }

    void hostEncoderOverrideDecisions()
    {
        // 1. HEC_SOFTWARE bypasses hardware packed YCoCg-R without warning even when hw_ycocgr_444 is advertised
        {
            Packed444Inputs in = makeValidPackedInputs();
            in.hostEncoderConfig = StreamingPreferences::HEC_SOFTWARE;
            in.hostNativeYuv444Supported = false;
            const Packed444Decision d = resolvePacked444Decision(in);
            QVERIFY(!d.usePackedYCoCgRA1);
            QCOMPARE(d.fallbackReason, QStringLiteral("host_software_encoder"));
            QCOMPARE(d.userWarning, QString());
        }

        // 2. HEC_HARDWARE_VULKAN on hw_ycocgr_444 host falls back with actionable warning
        {
            Packed444Inputs in = makeValidPackedInputs();
            in.hostEncoderConfig = StreamingPreferences::HEC_HARDWARE_VULKAN;
            const Packed444Decision d = resolvePacked444Decision(in);
            QVERIFY(!d.usePackedYCoCgRA1);
            QCOMPARE(d.fallbackReason, QStringLiteral("host_encoder_vulkan"));
            QVERIFY(d.userWarning.contains(QStringLiteral("Automatic or VAAPI host encoder")));
        }

        // 3. HEC_HARDWARE_VULKAN on legacy host returns host_unsupported silently
        {
            Packed444Inputs in = makeValidPackedInputs();
            in.hostEncoderConfig = StreamingPreferences::HEC_HARDWARE_VULKAN;
            in.hostSupportsHwYCoCgR444 = false;
            const Packed444Decision d = resolvePacked444Decision(in);
            QVERIFY(!d.usePackedYCoCgRA1);
            QCOMPARE(d.fallbackReason, QStringLiteral("host_unsupported"));
            QCOMPARE(d.userWarning, QString());
        }

        // 4. HEC_HARDWARE_VAAPI enables packed YCoCg-R cleanly
        {
            Packed444Inputs in = makeValidPackedInputs();
            in.hostEncoderConfig = StreamingPreferences::HEC_HARDWARE_VAAPI;
            in.rendererSelection = StreamingPreferences::RS_METAL;
            const Packed444Decision d = resolvePacked444Decision(in);
            QVERIFY(d.usePackedYCoCgRA1);
            QVERIFY(d.forceHevcMain10);
            QVERIFY(d.keep444Bitrate);
            QCOMPARE(d.userWarning, QString());
        }
    }

    void ycocgRP010RoundTripIsLossless()
    {
        auto verifyPixel = [](uint8_t r, uint8_t g, uint8_t b) {
            const Packed10Sample s = encodeYCoCgR10(r, g, b);

            // Simulate VideoToolbox P010 MSB [15:6] R16Unorm readback
            const uint16_t msb16 = static_cast<uint16_t>(s.y10 << 6);
            const uint16_t coMsb16 = static_cast<uint16_t>(s.co10 << 6);
            const uint16_t cgMsb16 = static_cast<uint16_t>(s.cg10 << 6);
            const float vtScale = 65535.0f / 64.0f;
            const uint16_t y10FromVt = static_cast<uint16_t>(std::rint((msb16 / 65535.0f) * vtScale));
            const uint16_t co10FromVt = static_cast<uint16_t>(std::rint((coMsb16 / 65535.0f) * vtScale));
            const uint16_t cg10FromVt = static_cast<uint16_t>(std::rint((cgMsb16 / 65535.0f) * vtScale));

            uint8_t rr = 0, gg = 0, bb = 0;
            decodeYCoCgR10(y10FromVt, co10FromVt, cg10FromVt, rr, gg, bb);
            QCOMPARE(rr, r);
            QCOMPARE(gg, g);
            QCOMPARE(bb, b);

            // Simulate software YUV420P10LE LSB [9:0] R16Unorm readback
            const uint16_t y10FromSw = static_cast<uint16_t>(std::rint((s.y10 / 65535.0f) * 65535.0f));
            const uint16_t co10FromSw = static_cast<uint16_t>(std::rint((s.co10 / 65535.0f) * 65535.0f));
            const uint16_t cg10FromSw = static_cast<uint16_t>(std::rint((s.cg10 / 65535.0f) * 65535.0f));
            decodeYCoCgR10(y10FromSw, co10FromSw, cg10FromSw, rr, gg, bb);
            QCOMPARE(rr, r);
            QCOMPARE(gg, g);
            QCOMPARE(bb, b);
        };

        for (int v = 0; v <= 255; ++v) {
            verifyPixel(static_cast<uint8_t>(v), static_cast<uint8_t>(v), static_cast<uint8_t>(v));
        }

        const uint8_t corners[][3] = {
            {0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
            {255, 255, 0}, {255, 0, 255}, {0, 255, 255},
            {0xFF, 0x20, 0x20}, {0x20, 0xFF, 0x20}, {0x30, 0x90, 0xFF},
            {0x1E, 0x1E, 0x1E}, {1, 254, 127}, {128, 127, 129},
        };
        for (const auto& c : corners) {
            verifyPixel(c[0], c[1], c[2]);
        }

        // Dense grid check across all RGB channels
        for (int r = 0; r <= 255; r += 17) {
            for (int g = 0; g <= 255; g += 17) {
                for (int b = 0; b <= 255; b += 17) {
                    verifyPixel(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
                }
            }
        }
    }
};

int runPacked444Tests(int argc, char** argv)
{
    TestPacked444 tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_packed444.moc"
