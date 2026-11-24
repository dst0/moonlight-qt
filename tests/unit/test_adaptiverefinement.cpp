#include "streaming/video/adaptiverefinement.h"
#include "streaming/video/adaptiverefinement_codec.h"
#include "tests.h"

#include <QtTest>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void appendU16LE(QByteArray& dst, std::uint16_t v)
{
    dst.append(static_cast<char>(v & 0xFFu));
    dst.append(static_cast<char>((v >> 8) & 0xFFu));
}

void appendU32LE(QByteArray& dst, std::uint32_t v)
{
    dst.append(static_cast<char>(v & 0xFFu));
    dst.append(static_cast<char>((v >> 8) & 0xFFu));
    dst.append(static_cast<char>((v >> 16) & 0xFFu));
    dst.append(static_cast<char>((v >> 24) & 0xFFu));
}

QByteArray buildBatchBinary(std::uint16_t flags,
                            std::uint16_t frameW,
                            std::uint16_t frameH,
                            const std::vector<std::array<std::uint16_t, 4>>& rects,
                            const std::vector<std::vector<std::uint8_t>>& payloads)
{
    QByteArray out;
    out.append("SDTR", 4);
    appendU16LE(out, AdaptiveRefinementWire::PROTOCOL_VERSION);
    appendU16LE(out, flags);
    appendU16LE(out, frameW);
    appendU16LE(out, frameH);
    appendU16LE(out, static_cast<std::uint16_t>(payloads.size()));
    appendU16LE(out, 0);

    for (std::size_t i = 0; i < payloads.size(); ++i) {
        appendU32LE(out, static_cast<std::uint32_t>(i + 1));
        appendU16LE(out, rects[i][0]);
        appendU16LE(out, rects[i][1]);
        appendU16LE(out, rects[i][2]);
        appendU16LE(out, rects[i][3]);
        appendU32LE(out, static_cast<std::uint32_t>(payloads[i].size()));
        out.append(reinterpret_cast<const char*>(payloads[i].data()),
                   static_cast<qsizetype>(payloads[i].size()));
    }
    return out;
}

} // namespace

class TestAdaptiveRefinement : public QObject
{
    Q_OBJECT

private slots:
    void qoiRoundTripsVariousSizesAndOps()
    {
        struct Case { int w; int h; };
        const std::array<Case, 3> cases{{{1, 1}, {19, 13}, {64, 64}}};
        for (const Case& c : cases) {
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(c.w) * c.h * 3u);
            for (int y = 0; y < c.h; ++y) {
                for (int x = 0; x < c.w; ++x) {
                    const std::size_t idx = (static_cast<std::size_t>(y) * c.w + x) * 3u;
                    if (y < c.h / 3 && (x % 7) != 0) {
                        rgb[idx + 0] = 30;
                        rgb[idx + 1] = 34;
                        rgb[idx + 2] = 42;
                    } else if (x % 2 == 0) {
                        rgb[idx + 0] = static_cast<std::uint8_t>((40 + x) & 0xFF);
                        rgb[idx + 1] = static_cast<std::uint8_t>((50 + y) & 0xFF);
                        rgb[idx + 2] = static_cast<std::uint8_t>((60 + x - y) & 0xFF);
                    } else {
                        rgb[idx + 0] = static_cast<std::uint8_t>((x * 91 + y * 13) & 0xFF);
                        rgb[idx + 1] = static_cast<std::uint8_t>((x * 37 + y * 83) & 0xFF);
                        rgb[idx + 2] = static_cast<std::uint8_t>((x * 17 + y * 59) & 0xFF);
                    }
                }
            }
            std::vector<std::uint8_t> qoi;
            QVERIFY(encodeQoiRgb24(rgb.data(), c.w * 3, c.w, c.h, qoi));
            std::vector<std::uint8_t> decoded;
            QVERIFY(decodeQoiRgb24(qoi.data(), qoi.size(), c.w, c.h, decoded));
            QCOMPARE(decoded, rgb);
        }
    }

    void qoiRejectsMalformedTruncatedOversizedAndRgba()
    {
        std::vector<std::uint8_t> rgb(64 * 64 * 3, 42);
        rgb[0] = 255;
        std::vector<std::uint8_t> qoi;
        QVERIFY(encodeQoiRgb24(rgb.data(), 64 * 3, 64, 64, qoi));

        std::vector<std::uint8_t> out;
        QVERIFY(!decodeQoiRgb24(qoi.data(), qoi.size() - 1, 64, 64, out));
        QVERIFY(!decodeQoiRgb24(qoi.data(), qoi.size(), 63, 64, out));
        QVERIFY(!decodeQoiRgb24(qoi.data(), qoi.size(), 65, 64, out));
        QVERIFY(!decodeQoiRgb24(qoi.data(), qoi.size(), 0, 64, out));

        auto badMagic = qoi;
        badMagic[0] = 'X';
        QVERIFY(!decodeQoiRgb24(badMagic.data(), badMagic.size(), 64, 64, out));

        auto badChannels = qoi;
        badChannels[12] = 4;
        QVERIFY(!decodeQoiRgb24(badChannels.data(), badChannels.size(), 64, 64, out));

        auto badEnd = qoi;
        badEnd.back() = 9;
        QVERIFY(!decodeQoiRgb24(badEnd.data(), badEnd.size(), 64, 64, out));

        auto trailing = qoi;
        trailing.push_back(0);
        QVERIFY(!decodeQoiRgb24(trailing.data(), trailing.size(), 64, 64, out));

        const std::vector<std::uint8_t> rgbaStream{
            'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0,
            0xFF, 10, 20, 30, 255,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        QVERIFY(!decodeQoiRgb24(rgbaStream.data(), rgbaStream.size(), 1, 1, out));

        std::vector<std::uint8_t> oversized(AdaptiveRefinementWire::MAX_TILE_PAYLOAD + 1, 0);
        QVERIFY(!decodeQoiRgb24(oversized.data(), oversized.size(), 64, 64, out));
    }

    void batchParserValidatesBoundsAndComputesYRef()
    {
        // Aligned partial edge tile on non-multiple-of-64 frame (1900x1060 -> cell at 1856,1024 has size 44x36)
        constexpr int tileW = 44;
        constexpr int tileH = 36;
        std::vector<std::uint8_t> rgb(tileW * tileH * 3);
        for (std::size_t i = 0; i < tileW * tileH; ++i) {
            rgb[i * 3 + 0] = 250;
            rgb[i * 3 + 1] = 30;
            rgb[i * 3 + 2] = 40;
        }
        std::vector<std::uint8_t> qoi;
        QVERIFY(encodeQoiRgb24(rgb.data(), tileW * 3, tileW, tileH, qoi));

        const QByteArray validBin = buildBatchBinary(
            AdaptiveRefinementWire::FLAG_RESET_ALL, 1900, 1060,
            {{{1856, 1024, tileW, tileH}}}, {qoi});

        AdaptiveTileBatch batch;
        QVERIFY(parseTileRefinementBatch(validBin, RefinementColorspace::Rec709, false, batch));
        QVERIFY(batch.valid);
        QVERIFY(batch.resetAll);
        QCOMPARE(batch.frameW, std::uint16_t(1900));
        QCOMPARE(batch.frameH, std::uint16_t(1060));
        QCOMPARE(batch.tiles.size(), std::size_t(1));
        const auto* px = reinterpret_cast<const std::uint8_t*>(batch.tiles[0].rgba.constData());
        QCOMPARE(px[0], std::uint8_t(250));
        QCOMPARE(px[1], std::uint8_t(30));
        QCOMPARE(px[2], std::uint8_t(40));
        QCOMPARE(px[3], computeRefinementYRef(250, 30, 40, RefinementColorspace::Rec709, false));

        // Unaligned coordinates must be rejected even if within bounds
        const QByteArray unalignedBin = buildBatchBinary(0, 1900, 1060, {{{1855, 1024, tileW, tileH}}}, {qoi});
        QVERIFY(!parseTileRefinementBatch(unalignedBin, RefinementColorspace::Rec601, true, batch));

        // Partial interior coverage (44x36 at 0,0 inside 1900x1060 instead of 64x64) must be rejected
        const QByteArray underSizedBin = buildBatchBinary(0, 1900, 1060, {{{0, 0, tileW, tileH}}}, {qoi});
        QVERIFY(!parseTileRefinementBatch(underSizedBin, RefinementColorspace::Rec601, true, batch));

        // Out-of-bounds tile (x + w > frameW) must fail cleanly
        const QByteArray oobBin = buildBatchBinary(0, 1900, 1060, {{{1888, 1024, tileW, tileH}}}, {qoi});
        QVERIFY(!parseTileRefinementBatch(oobBin, RefinementColorspace::Rec601, true, batch));

        // Truncated batch must fail cleanly
        QVERIFY(!parseTileRefinementBatch(validBin.left(validBin.size() - 1), RefinementColorspace::Rec601, true, batch));

        // Bad magic
        QByteArray badMagic = validBin;
        badMagic[0] = 'Z';
        QVERIFY(!parseTileRefinementBatch(badMagic, RefinementColorspace::Rec601, true, batch));

        // Oversized (> 256 KiB)
        QByteArray tooBig(AdaptiveRefinementWire::MAX_BATCH_BYTES + 1, '\0');
        QVERIFY(!parseTileRefinementBatch(tooBig, RefinementColorspace::Rec601, true, batch));
    }

    void inboxCoalescesCapsAndRecomputesColorimetry()
    {
        AdaptiveTileInbox inbox;
        inbox.setColorimetry(RefinementColorspace::Rec601, true);

        std::vector<std::uint8_t> rgbA(64 * 64 * 3, 200);
        std::vector<std::uint8_t> qoiA;
        QVERIFY(encodeQoiRgb24(rgbA.data(), 64 * 3, 64, 64, qoiA));

        AdaptiveTileBatch b1;
        QVERIFY(parseTileRefinementBatch(
            buildBatchBinary(0, 2048, 2048, {{{0, 0, 64, 64}}}, {qoiA}),
            RefinementColorspace::Rec601, true, b1));
        inbox.pushBatch(std::move(b1));

        // Coalesce update to same coordinate (0, 0)
        std::vector<std::uint8_t> rgbB(64 * 64 * 3, 0);
        for (std::size_t i = 0; i < 64 * 64; ++i) {
            rgbB[i * 3 + 0] = 240;
            rgbB[i * 3 + 1] = 20;
            rgbB[i * 3 + 2] = 60;
        }
        std::vector<std::uint8_t> qoiB;
        QVERIFY(encodeQoiRgb24(rgbB.data(), 64 * 3, 64, 64, qoiB));
        AdaptiveTileBatch b2;
        QVERIFY(parseTileRefinementBatch(
            buildBatchBinary(0, 2048, 2048, {{{0, 0, 64, 64}}}, {qoiB}),
            RefinementColorspace::Rec601, true, b2));
        inbox.pushBatch(std::move(b2));
        QCOMPARE(inbox.queuedTileCount(), std::size_t(1));

        // Draining under Rec709 limited range recomputes Y_ref accurately
        bool resetAll = false;
        std::uint16_t fw = 0, fh = 0;
        std::vector<AdaptiveDecodedTile> outTiles;
        QVERIFY(inbox.takePending(resetAll, fw, fh, outTiles, RefinementColorspace::Rec709, false));
        QVERIFY(!resetAll);
        QCOMPARE(fw, std::uint16_t(2048));
        QCOMPARE(fh, std::uint16_t(2048));
        QCOMPARE(outTiles.size(), std::size_t(1));
        const auto* px = reinterpret_cast<const std::uint8_t*>(outTiles[0].rgba.constData());
        QCOMPARE(px[3], computeRefinementYRef(240, 20, 60, RefinementColorspace::Rec709, false));

        // Subsequent batch at changed resolution (1280x720) without FLAG_RESET_ALL still triggers reset
        AdaptiveTileBatch bResChanged;
        QVERIFY(parseTileRefinementBatch(
            buildBatchBinary(0, 1280, 720, {{{0, 0, 64, 64}}}, {qoiA}),
            RefinementColorspace::Rec709, false, bResChanged));
        inbox.pushBatch(std::move(bResChanged));
        QVERIFY(inbox.takePending(resetAll, fw, fh, outTiles, RefinementColorspace::Rec709, false));
        QVERIFY(resetAll);
        QCOMPARE(fw, std::uint16_t(1280));
        QCOMPARE(fh, std::uint16_t(720));
        QCOMPARE(outTiles.size(), std::size_t(1));
    }

    void resolveDecisionEnforcesAllSettingsEdgeCases()
    {
        AdaptiveRefinementInputs base;
        base.hostToggleEnabled = false;
        base.hostSupportsTileRefinement = true;
        base.enableHdr = false;
        base.enableYUV444 = false;
        base.isMacOS = true;
        base.isMetalAvailable = true;
        base.rendererSelection = StreamingPreferences::RS_AUTO;

        // 1. Off by default
        {
            const auto d = resolveAdaptiveRefinementDecision(base);
            QVERIFY(!d.active);
            QVERIFY(!d.suppressPackedYCoCgR);
            QVERIFY(!d.suppressNativeYuv444);
        }

        // 2. Host lacks adaptive_tile_refinement
        {
            auto in = base;
            in.hostToggleEnabled = true;
            in.hostSupportsTileRefinement = false;
            const auto d = resolveAdaptiveRefinementDecision(in);
            QVERIFY(!d.active);
            QCOMPARE(d.fallbackReason, QStringLiteral("host_missing_adaptive_tile_refinement"));
            QVERIFY(!d.statusNote.isEmpty());
        }

        // 3. HDR conflict
        {
            auto in = base;
            in.hostToggleEnabled = true;
            in.enableHdr = true;
            const auto d = resolveAdaptiveRefinementDecision(in);
            QVERIFY(!d.active);
            QCOMPARE(d.fallbackReason, QStringLiteral("hdr_conflict"));
            QVERIFY(d.statusNote.contains(QStringLiteral("HDR")));
        }

        // 4. Non-Metal renderer conflict (Vulkan / AVSBDL / non-macOS)
        for (auto r : {StreamingPreferences::RS_VULKAN, StreamingPreferences::RS_AVSBDL}) {
            auto in = base;
            in.hostToggleEnabled = true;
            in.rendererSelection = r;
            const auto d = resolveAdaptiveRefinementDecision(in);
            QVERIFY(!d.active);
            QCOMPARE(d.fallbackReason, QStringLiteral("non_metal_renderer"));
        }
        {
            auto in = base;
            in.hostToggleEnabled = true;
            in.isMacOS = false;
            const auto d = resolveAdaptiveRefinementDecision(in);
            QVERIFY(!d.active);
            QCOMPARE(d.fallbackReason, QStringLiteral("non_metal_renderer"));
        }

        // 5. Valid -> active + suppresses packed/native 4:4:4 + forces Metal
        {
            auto in = base;
            in.hostToggleEnabled = true;
            const auto d = resolveAdaptiveRefinementDecision(in);
            QVERIFY(d.active);
            QVERIFY(d.suppressPackedYCoCgR);
            QVERIFY(d.suppressNativeYuv444);
            QVERIFY(d.forceMetalRenderer);
            QVERIFY(d.statusNote.isEmpty());
        }

        // 6. Valid + enableYUV444 also checked -> active with explanatory override note
        {
            auto in = base;
            in.hostToggleEnabled = true;
            in.enableYUV444 = true;
            in.rendererSelection = StreamingPreferences::RS_METAL;
            const auto d = resolveAdaptiveRefinementDecision(in);
            QVERIFY(d.active);
            QVERIFY(d.suppressPackedYCoCgR);
            QVERIFY(d.suppressNativeYuv444);
            QVERIFY(d.forceMetalRenderer);
            QVERIFY(d.statusNote.contains(QStringLiteral("4:4:4")));
        }
    }
};

int runAdaptiveRefinementTests(int argc, char** argv)
{
    TestAdaptiveRefinement tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_adaptiverefinement.moc"
