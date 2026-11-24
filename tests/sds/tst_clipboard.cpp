// S13: clipboard validation (invalid UTF-8, NUL, bad PNG header, oversized), echo suppression,
// and the NSPasteboard backend on a private pasteboard (never the user's clipboard).

#include "streaming/sds/clipboard_platform.h"
#include "streaming/sds/clipboardsync.h"
#include "testutil.h"

#include <QTest>
#include <QUuid>

using namespace Sds;

Q_DECLARE_METATYPE(Sds::ClipboardCheck)

#ifdef Q_OS_DARWIN
// pasteboard_helper.mm
bool sdsTestWriteTiffOnly(const QString& pasteboardName, const QByteArray& png);
QStringList sdsTestPasteboardTypes(const QString& pasteboardName);
#endif

class TestClipboard : public QObject
{
    Q_OBJECT

private slots:
    void text_data();
    void text();
    void png_data();
    void png();
    void pngReportsDimensions();
    void sanitizeItemDropsInvalidParts();
    void echoGuard();
    void contentDedupe();
    void platformBackendRoundTrip();
    void platformBackendTiffFallback();
    void platformBackendLimits();
};

void TestClipboard::text_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<qint64>("max");
    QTest::addColumn<Sds::ClipboardCheck>("expected");

    QTest::newRow("ok") << QByteArray("hello") << qint64(100) << ClipboardCheck::Ok;
    QTest::newRow("utf8 ok") << QByteArray("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80") << qint64(100) << ClipboardCheck::Ok;
    QTest::newRow("empty") << QByteArray() << qint64(100) << ClipboardCheck::Empty;
    QTest::newRow("exactly max") << QByteArray(16, 'a') << qint64(16) << ClipboardCheck::Ok;
    QTest::newRow("over max") << QByteArray(17, 'a') << qint64(16) << ClipboardCheck::TooLarge;
    QTest::newRow("nul") << QByteArray("a\0b", 3) << qint64(100) << ClipboardCheck::ContainsNul;
    QTest::newRow("invalid lead") << QByteArray("abc\xFF") << qint64(100) << ClipboardCheck::InvalidUtf8;
    QTest::newRow("overlong") << QByteArray("\xC0\xAF") << qint64(100) << ClipboardCheck::InvalidUtf8;
    QTest::newRow("surrogate") << QByteArray("\xED\xB2\x80") << qint64(100) << ClipboardCheck::InvalidUtf8;
    QTest::newRow("truncated") << QByteArray("\xF0\x9F\x98") << qint64(100) << ClipboardCheck::InvalidUtf8;
}

void TestClipboard::text()
{
    QFETCH(QByteArray, data);
    QFETCH(qint64, max);
    QFETCH(Sds::ClipboardCheck, expected);
    QCOMPARE(validateClipboardText(data, max), expected);
}

void TestClipboard::png_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<qint64>("max");
    QTest::addColumn<Sds::ClipboardCheck>("expected");

    const qint64 big = 64LL * 1024 * 1024;
    QByteArray good = SdsTest::makePng(4, 3);

    QTest::newRow("valid") << good << big << ClipboardCheck::Ok;
    QTest::newRow("header only") << SdsTest::makePngHeader(10, 10) << big << ClipboardCheck::Ok;
    QTest::newRow("empty") << QByteArray() << big << ClipboardCheck::Empty;
    QTest::newRow("too many bytes") << good << qint64(good.size() - 1) << ClipboardCheck::TooLarge;
    QTest::newRow("bad signature") << (QByteArray("\x89PNX") + good.mid(4)) << big << ClipboardCheck::BadPng;
    QTest::newRow("jpeg") << QByteArray::fromHex("ffd8ffe000104a46494600010100000100010000ffdb004300080606070606050806070707") << big << ClipboardCheck::BadPng;
    QTest::newRow("truncated") << good.left(20) << big << ClipboardCheck::BadPng;
    QTest::newRow("bad crc") << SdsTest::makePngHeader(10, 10, 8, 6, true) << big << ClipboardCheck::BadPng;
    QTest::newRow("zero width") << SdsTest::makePngHeader(0, 10) << big << ClipboardCheck::BadPng;
    QTest::newRow("huge width") << SdsTest::makePngHeader(0x80000000u, 1) << big << ClipboardCheck::BadPng;
    QTest::newRow("bad colour type") << SdsTest::makePngHeader(10, 10, 8, 5) << big << ClipboardCheck::BadPng;
    QTest::newRow("palette 16 bit") << SdsTest::makePngHeader(10, 10, 16, 3) << big << ClipboardCheck::BadPng;
    QTest::newRow("rgb 4 bit") << SdsTest::makePngHeader(10, 10, 4, 2) << big << ClipboardCheck::BadPng;
    QTest::newRow("grey 1 bit") << SdsTest::makePngHeader(10, 10, 1, 0) << big << ClipboardCheck::Ok;
    QTest::newRow("dims at limit") << SdsTest::makePngHeader(8192, 8192) << big << ClipboardCheck::Ok;      // 256 MiB
    QTest::newRow("dims over limit") << SdsTest::makePngHeader(8192, 8193) << big << ClipboardCheck::PngTooLarge;
    QTest::newRow("decompression bomb dims") << SdsTest::makePngHeader(100000, 100000) << big << ClipboardCheck::PngTooLarge;

    QByteArray wrongIhdrLength = good;
    wrongIhdrLength[11] = 14;
    QTest::newRow("ihdr length") << wrongIhdrLength << big << ClipboardCheck::BadPng;

    QByteArray notIhdr = good;
    notIhdr[12] = 'X';
    QTest::newRow("first chunk not ihdr") << notIhdr << big << ClipboardCheck::BadPng;
}

void TestClipboard::png()
{
    QFETCH(QByteArray, data);
    QFETCH(qint64, max);
    QFETCH(Sds::ClipboardCheck, expected);
    QCOMPARE(validateClipboardPng(data, max), expected);
}

void TestClipboard::pngReportsDimensions()
{
    quint32 w = 0, h = 0;
    QCOMPARE(validateClipboardPng(SdsTest::makePng(7, 5), 1 << 20, &w, &h), ClipboardCheck::Ok);
    QCOMPARE(w, 7u);
    QCOMPARE(h, 5u);
}

void TestClipboard::sanitizeItemDropsInvalidParts()
{
    Limits limits;
    limits.clipboardTextMax = 8;
    limits.clipboardImageMax = 1 << 20;

    ClipboardItem item;
    item.text = QByteArray("way too long text");
    item.png = SdsTest::makePng(2, 2);
    ClipboardCheck tc, ic;
    sanitizeClipboardItem(item, limits, &tc, &ic);
    QCOMPARE(tc, ClipboardCheck::TooLarge);
    QCOMPARE(ic, ClipboardCheck::Ok);
    QVERIFY(!item.hasText());
    QVERIFY(item.hasImage());

    item.text = QByteArray("ok");
    item.png = QByteArray("not a png at all, really not");
    sanitizeClipboardItem(item, limits, &tc, &ic);
    QCOMPARE(tc, ClipboardCheck::Ok);
    QCOMPARE(ic, ClipboardCheck::BadPng);
    QVERIFY(item.hasText());
    QVERIFY(!item.hasImage());

    QVERIFY(!describeClipboardCheck(ClipboardCheck::TooLarge, true).isEmpty());
    QVERIFY(describeClipboardCheck(ClipboardCheck::Ok, true).isEmpty());
}

void TestClipboard::echoGuard()
{
    ClipboardEchoGuard g;
    g.reset(5);
    QVERIFY(!g.pollChanged(5));   // baseline is not a change
    QVERIFY(g.pollChanged(6));    // user copied something
    QVERIFY(!g.pollChanged(6));   // reported once
    g.noteOwnWrite(7);            // we wrote host content
    QVERIFY(!g.pollChanged(7));   // our own write is not pushed back
    QVERIFY(g.pollChanged(8));    // the next user copy is
    g.noteOwnWrite(9);
    QVERIFY(g.pollChanged(10));   // a change right after our write still counts
    QVERIFY(!g.pollChanged(10));
    QCOMPARE(g.lastSeen(), qint64(10));
    g.reset(20);                  // a new baseline forgets our last write
    QVERIFY(!g.pollChanged(20));
    QVERIFY(g.pollChanged(9));
}

void TestClipboard::contentDedupe()
{
    ClipboardEchoGuard g;
    ClipboardItem a;
    a.text = "same";
    ClipboardItem b;
    b.text = "other";
    ClipboardItem textAndImage;
    textAndImage.text = "same";
    textAndImage.png = SdsTest::makePng(1, 1);

    QVERIFY(!g.isDuplicateOfLastSynced(a));
    g.noteSynced(a);
    QVERIFY(g.isDuplicateOfLastSynced(a));
    QVERIFY(!g.isDuplicateOfLastSynced(b));
    QVERIFY(!g.isDuplicateOfLastSynced(textAndImage));

    // Representation boundaries are part of the hash
    ClipboardItem split1;
    split1.text = "ab";
    ClipboardItem split2;
    split2.text = "a";
    split2.png = "b";
    QVERIFY(ClipboardEchoGuard::contentHash(split1) != ClipboardEchoGuard::contentHash(split2));
}

void TestClipboard::platformBackendRoundTrip()
{
#ifndef Q_OS_DARWIN
    QSKIP("NSPasteboard backend is macOS only");
#else
    const QString name = QStringLiteral("com.moonlight-stream.sds-test.") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto cb = createPlatformClipboard(name);
    QVERIFY(cb->isAvailable());

    ClipboardItem item;
    item.text = QByteArray("Hello \xF0\x9F\x8C\x8D from the host");
    item.png = SdsTest::makePng(16, 9);

    qint64 before = cb->changeCount();
    qint64 after = cb->write(item);
    QVERIFY(after != before);
    QCOMPARE(cb->changeCount(), after); // the returned count is the current one

    IClipboardBackend::ReadResult r = cb->read(1 << 20, 16 << 20);
    QCOMPARE(r.item.text, item.text);
    QCOMPARE(r.item.png, item.png); // PNG is preferred over the TIFF copy
    QVERIFY(!r.textTooLarge && !r.imageTooLarge);

    // Text only replaces the image
    ClipboardItem textOnly;
    textOnly.text = "plain";
    cb->write(textOnly);
    r = cb->read(1 << 20, 16 << 20);
    QCOMPARE(r.item.text, QByteArray("plain"));
    QVERIFY(r.item.png.isEmpty());
#endif
}

void TestClipboard::platformBackendTiffFallback()
{
#ifndef Q_OS_DARWIN
    QSKIP("NSPasteboard backend is macOS only");
#else
    // Another app put only a TIFF on the pasteboard: the backend converts it to PNG
    const QString name = QStringLiteral("com.moonlight-stream.sds-test.") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto cb = createPlatformClipboard(name);
    QVERIFY(sdsTestWriteTiffOnly(name, SdsTest::makePng(32, 20)));

    IClipboardBackend::ReadResult r = cb->read(1 << 20, 16 << 20);
    QVERIFY(r.item.text.isEmpty());
    quint32 w = 0, h = 0;
    QCOMPARE(validateClipboardPng(r.item.png, 16 << 20, &w, &h), ClipboardCheck::Ok);
    QCOMPARE(w, 32u);
    QCOMPARE(h, 20u);

    // Writing host content puts both PNG and TIFF on the pasteboard
    ClipboardItem item;
    item.png = SdsTest::makePng(8, 8);
    cb->write(item);
    QStringList types = sdsTestPasteboardTypes(name);
    QVERIFY(types.contains(QStringLiteral("public.png")));
    QVERIFY(types.contains(QStringLiteral("public.tiff")));
#endif
}

void TestClipboard::platformBackendLimits()
{
#ifndef Q_OS_DARWIN
    QSKIP("NSPasteboard backend is macOS only");
#else
    const QString name = QStringLiteral("com.moonlight-stream.sds-test.") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto cb = createPlatformClipboard(name);

    ClipboardItem item;
    item.text = QByteArray(4096, 'x');
    item.png = SdsTest::makePng(64, 64);
    cb->write(item);

    IClipboardBackend::ReadResult r = cb->read(100, 100);
    QVERIFY(r.textTooLarge);
    QVERIFY(r.imageTooLarge);
    QVERIFY(r.item.isEmpty()); // dropped, never truncated
#endif
}

int runClipboardTests(int argc, char** argv)
{
    TestClipboard t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_clipboard.moc"
