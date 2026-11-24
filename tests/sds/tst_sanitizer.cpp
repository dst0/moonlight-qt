// S7: file name sanitization (DESIGN 6.4), collision names (S9).

#include "streaming/sds/filesanitize.h"

#include <QTest>

using namespace Sds;

class TestSanitizer : public QObject
{
    Q_OBJECT

private slots:
    void table_data();
    void table();
    void idempotent_data() { table_data(); }
    void idempotent();
    void overlong();
    void utf8Bytes_data();
    void utf8Bytes();
    void collisionNames();
    void collisionNameStaysWithinLimit();
};

void TestSanitizer::table_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected"); // null QString = rejected

    QTest::newRow("plain") << QStringLiteral("report.pdf") << QStringLiteral("report.pdf");
    QTest::newRow("unicode") << QStringLiteral("Отчёт 2026.docx") << QStringLiteral("Отчёт 2026.docx");
    QTest::newRow("empty") << QString() << QString();
    QTest::newRow("dot") << QStringLiteral(".") << QString();
    QTest::newRow("dotdot") << QStringLiteral("..") << QString();
    QTest::newRow("only dots") << QStringLiteral("...") << QString();
    QTest::newRow("only spaces") << QStringLiteral("   ") << QString();
    QTest::newRow("traversal slash") << QStringLiteral("../etc/passwd") << QStringLiteral("_._etc_passwd");
    QTest::newRow("traversal backslash") << QStringLiteral("..\\..\\boot.ini") << QStringLiteral("_._.._boot.ini");
    QTest::newRow("absolute") << QStringLiteral("/etc/passwd") << QStringLiteral("_etc_passwd");
    QTest::newRow("reserved chars") << QStringLiteral("a/b\\c:d*e?f\"g<h>i|j") << QStringLiteral("a_b_c_d_e_f_g_h_i_j");
    QTest::newRow("nul") << (QStringLiteral("nul") + QChar(0) + QStringLiteral("byte")) << QStringLiteral("nul_byte");
    QTest::newRow("c0 controls") << QStringLiteral("tab\tnew\nline\r") << QStringLiteral("tab_new_line_");
    QTest::newRow("del") << (QStringLiteral("a") + QChar(0x7F) + QStringLiteral("b")) << QStringLiteral("a_b");
    QTest::newRow("c1 control") << (QChar(0x85) + QStringLiteral("c1") + QChar(0x9F)) << QStringLiteral("_c1_");
    QTest::newRow("bidi override") << (QStringLiteral("photo") + QChar(0x202E) + QStringLiteral("gpj.exe")) << QStringLiteral("photo_gpj.exe");
    QTest::newRow("bidi isolate") << (QChar(0x2067) + QStringLiteral("x.txt")) << QStringLiteral("_x.txt");
    QTest::newRow("leading dot") << QStringLiteral(".bashrc") << QStringLiteral("_bashrc");
    QTest::newRow("leading dash") << QStringLiteral("-rf") << QStringLiteral("_rf");
    QTest::newRow("leading double dash") << QStringLiteral("--help") << QStringLiteral("_-help");
    QTest::newRow("leading dots") << QStringLiteral("..hidden") << QStringLiteral("_.hidden");
    QTest::newRow("trailing dots spaces") << QStringLiteral("name. . .") << QStringLiteral("name");
    QTest::newRow("trailing spaces") << QStringLiteral("notes.txt   ") << QStringLiteral("notes.txt");
    QTest::newRow("reserved CON") << QStringLiteral("CON") << QStringLiteral("_CON");
    QTest::newRow("reserved con.txt") << QStringLiteral("con.txt") << QStringLiteral("_con.txt");
    QTest::newRow("reserved nul.tar.gz") << QStringLiteral("nul.tar.gz") << QStringLiteral("_nul.tar.gz");
    QTest::newRow("reserved LPT9") << QStringLiteral("LPT9.log") << QStringLiteral("_LPT9.log");
    QTest::newRow("reserved com1") << QStringLiteral("com1") << QStringLiteral("_com1");
    QTest::newRow("reserved superscript") << (QStringLiteral("COM") + QChar(0x00B9)) << (QStringLiteral("_COM") + QChar(0x00B9));
    QTest::newRow("reserved with space") << QStringLiteral("Aux .h") << QStringLiteral("_Aux .h");
    QTest::newRow("not reserved COM10") << QStringLiteral("COM10") << QStringLiteral("COM10");
    QTest::newRow("not reserved CONSOLE") << QStringLiteral("CONSOLE.txt") << QStringLiteral("CONSOLE.txt");
    QTest::newRow("not reserved COM0") << QStringLiteral("COM0") << QStringLiteral("COM0");
    QTest::newRow("nfd to nfc") << QStringLiteral("été.txt") << QStringLiteral("été.txt");
    QTest::newRow("nfd hangul") << QStringLiteral("한.txt") << QStringLiteral("한.txt");
    QTest::newRow("lone high surrogate") << (QStringLiteral("a") + QChar(0xD800) + QStringLiteral("b")) << QString();
    QTest::newRow("lone low surrogate") << (QChar(0xDC00) + QStringLiteral("b")) << QString();
    QTest::newRow("emoji kept") << QStringLiteral("smile \U0001F600.png") << QStringLiteral("smile \U0001F600.png");
    QTest::newRow("dot only extension") << QStringLiteral("a.") << QStringLiteral("a");
}

void TestSanitizer::table()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    QString got = sanitizeFileName(input);
    if (expected.isNull()) {
        QVERIFY2(got.isNull(), qPrintable(QStringLiteral("expected rejection, got '%1'").arg(got)));
    }
    else {
        QCOMPARE(got, expected);
        QVERIFY(got.toUtf8().size() <= kMaxNameBytes);
        QVERIFY(!got.contains(QLatin1Char('/')));
        QVERIFY(!got.contains(QLatin1Char('\\')));
        QVERIFY(!got.startsWith(QLatin1Char('.')));
        QVERIFY(!got.startsWith(QLatin1Char('-')));
        QCOMPARE(got, got.normalized(QString::NormalizationForm_C));
    }
}

void TestSanitizer::idempotent()
{
    QFETCH(QString, input);
    QString once = sanitizeFileName(input);
    if (!once.isNull()) {
        QCOMPARE(sanitizeFileName(once), once);
    }
}

void TestSanitizer::overlong()
{
    // ASCII stem, extension preserved
    QString longName = QString(250, QLatin1Char('a')) + QStringLiteral(".txt");
    QString got = sanitizeFileName(longName);
    QCOMPARE(got.toUtf8().size(), kMaxNameBytes);
    QVERIFY(got.endsWith(QStringLiteral(".txt")));
    QCOMPARE(got, QString(196, QLatin1Char('a')) + QStringLiteral(".txt"));

    // Two-byte characters are never split
    QString cyr = QString(150, QChar(0x0436)); // 300 bytes
    got = sanitizeFileName(cyr);
    QCOMPARE(got, QString(100, QChar(0x0436)));
    QCOMPARE(got.toUtf8().size(), 200);

    // A short extension is kept: 198 bytes of stem space -> 99 two-byte chars + ".x"
    QString mixed = QString(150, QChar(0x0436)) + QStringLiteral(".x");
    got = sanitizeFileName(mixed);
    QVERIFY(got.endsWith(QStringLiteral(".x")));
    QVERIFY(got.toUtf8().size() <= kMaxNameBytes);
    QCOMPARE(got.toUtf8().size(), 2 * 99 + 2);

    // Four-byte characters (surrogate pairs) are never split
    QString emoji;
    for (int i = 0; i < 60; i++) {
        emoji += QStringLiteral("\U0001F600"); // 240 bytes
    }
    got = sanitizeFileName(emoji);
    QCOMPARE(got.toUtf8().size(), 200);
    QVERIFY(QString::fromUtf8(got.toUtf8()) == got);

    // An extension longer than 32 bytes is not preserved
    QString longExt = QString(190, QLatin1Char('a')) + QLatin1Char('.') + QString(40, QLatin1Char('b'));
    got = sanitizeFileName(longExt);
    QCOMPARE(got.toUtf8().size(), 200);
    QVERIFY(got.startsWith(QString(190, QLatin1Char('a'))));

    // Truncation must not leave a trailing dot or space
    QString trailing = QString(199, QLatin1Char('a')) + QStringLiteral(" .") + QString(40, QLatin1Char('b'));
    got = sanitizeFileName(trailing);
    QVERIFY(!got.endsWith(QLatin1Char(' ')) && !got.endsWith(QLatin1Char('.')));
    QCOMPARE(sanitizeFileName(got), got);

    // Overlong names whose truncation exposes a Windows reserved stem must remain idempotent
    for (const QString& reserved : {
             QStringLiteral("CON") + QString(195, QLatin1Char(' ')) + QStringLiteral("x.txt"),
             QStringLiteral("CON") + QString(197, QLatin1Char(' ')) + QStringLiteral("a"),
             QStringLiteral("lpt1") + QString(194, QLatin1Char(' ')) + QStringLiteral("z.log"),
             QStringLiteral("aux") + QString(200, QLatin1Char('.')) + QStringLiteral("c"),
         }) {
        const QString once = sanitizeFileName(reserved);
        QVERIFY(!once.isEmpty());
        QVERIFY(once.toUtf8().size() <= kMaxNameBytes);
        QCOMPARE(sanitizeFileName(once), once);
    }
}

void TestSanitizer::utf8Bytes_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("valid") << QByteArray("caf\xC3\xA9.txt") << QStringLiteral("café.txt");
    QTest::newRow("nfd bytes") << QByteArray("cafe\xCC\x81.txt") << QStringLiteral("café.txt");
    QTest::newRow("overlong slash") << QByteArray("a\xC0\xAF" "b") << QString();
    QTest::newRow("overlong 3-byte") << QByteArray("\xE0\x80\xAF") << QString();
    QTest::newRow("surrogate") << QByteArray("\xED\xA0\x80") << QString();
    QTest::newRow("above max") << QByteArray("\xF4\x90\x80\x80") << QString();
    QTest::newRow("invalid lead") << QByteArray("abc\xFF") << QString();
    QTest::newRow("truncated") << QByteArray("abc\xE2\x82") << QString();
    QTest::newRow("stray continuation") << QByteArray("\x80" "abc") << QString();
    QTest::newRow("embedded nul") << QByteArray("a\0b", 3) << QStringLiteral("a_b");
}

void TestSanitizer::utf8Bytes()
{
    QFETCH(QByteArray, input);
    QFETCH(QString, expected);

    QString got = sanitizeFileNameUtf8(input);
    if (expected.isNull()) {
        QVERIFY(got.isNull());
        QVERIFY(!isValidUtf8(input));
    }
    else {
        QCOMPARE(got, expected);
    }
}

void TestSanitizer::collisionNames()
{
    QCOMPARE(collisionName(QStringLiteral("report.pdf"), 0), QStringLiteral("report.pdf"));
    QCOMPARE(collisionName(QStringLiteral("report.pdf"), 1), QStringLiteral("report (1).pdf"));
    QCOMPARE(collisionName(QStringLiteral("report.pdf"), 999), QStringLiteral("report (999).pdf"));
    QCOMPARE(collisionName(QStringLiteral("archive.tar.gz"), 2), QStringLiteral("archive.tar (2).gz"));
    QCOMPARE(collisionName(QStringLiteral("README"), 3), QStringLiteral("README (3)"));
    QCOMPARE(collisionName(QStringLiteral("_bashrc"), 1), QStringLiteral("_bashrc (1)"));
}

void TestSanitizer::collisionNameStaysWithinLimit()
{
    QString name = sanitizeFileName(QString(300, QChar(0x0436)) + QStringLiteral(".txt"));
    QVERIFY(name.toUtf8().size() <= kMaxNameBytes);
    for (int n : { 1, 10, 999 }) {
        QString c = collisionName(name, n);
        QVERIFY(c.toUtf8().size() <= kMaxNameBytes);
        QVERIFY(c.endsWith(QStringLiteral(" (%1).txt").arg(n)));
        QCOMPARE(sanitizeFileName(c), c);
    }
}

int runSanitizerTests(int argc, char** argv)
{
    TestSanitizer t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_sanitizer.moc"
