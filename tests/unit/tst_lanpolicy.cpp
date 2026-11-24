#include <QtTest>

#include "backend/lanpolicy.h"
#include "tests.h"

class TestLanPolicy : public QObject
{
    Q_OBJECT

private slots:
    void connectionTargets_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("permitted");
        for (const char* address : {"10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.10.20",
                                    "169.254.1.2", "127.0.0.1", "::1", "fe80::1%en0", "fd12::1",
                                    "::ffff:192.168.10.30", "::ffff:127.0.0.1"}) {
            QTest::newRow(address) << QString::fromLatin1(address) << true;
        }
        for (const char* address : {"", "localhost", "example.invalid", "0.0.0.0", "::", "8.8.8.8",
                                    "203.0.113.1", "100.64.0.1", "172.15.255.255", "172.32.0.1",
                                    "192.169.0.1", "255.255.255.255", "224.0.0.251", "ff02::1",
                                    "2001:db8::1", "2a00::1", "::ffff:8.8.8.8"}) {
            QTest::newRow(*address ? address : "empty") << QString::fromLatin1(address) << false;
        }
    }

    void connectionTargets()
    {
        QFETCH(QString, address);
        QFETCH(bool, permitted);
        QCOMPARE(LanPolicy::isPermittedConnectionTarget(QHostAddress(address)), permitted);
    }

    void wakeTargets_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<bool>("permitted");

        // RFC 1918 private IPv4, including range boundaries
        QTest::newRow("10/8 low") << "10.0.0.0" << true;
        QTest::newRow("10/8") << "10.1.2.3" << true;
        QTest::newRow("10/8 high") << "10.255.255.255" << true;
        QTest::newRow("172.16/12 low") << "172.16.0.0" << true;
        QTest::newRow("172.16/12 high") << "172.31.255.255" << true;
        QTest::newRow("below 172.16/12") << "172.15.255.255" << false;
        QTest::newRow("above 172.16/12") << "172.32.0.0" << false;
        QTest::newRow("192.168/16") << "192.168.1.10" << true;
        QTest::newRow("192.168/16 directed broadcast") << "192.168.1.255" << true;
        QTest::newRow("above 192.168/16") << "192.169.0.1" << false;
        QTest::newRow("below 192.168/16") << "192.167.255.255" << false;

        // IPv4 link-local and limited broadcast
        QTest::newRow("169.254/16") << "169.254.10.20" << true;
        QTest::newRow("outside 169.254/16") << "169.255.0.1" << false;
        QTest::newRow("limited broadcast") << "255.255.255.255" << true;

        // Public and special IPv4
        QTest::newRow("public 8.8.8.8") << "8.8.8.8" << false;
        QTest::newRow("public 1.1.1.1") << "1.1.1.1" << false;
        QTest::newRow("public 203.0.113.7") << "203.0.113.7" << false;
        QTest::newRow("CGNAT 100.64/10") << "100.64.0.1" << false;
        QTest::newRow("loopback") << "127.0.0.1" << false;
        QTest::newRow("unspecified v4") << "0.0.0.0" << false;
        QTest::newRow("mDNS multicast v4") << "224.0.0.251" << false;
        QTest::newRow("public directed broadcast") << "203.0.113.255" << false;

        // IPv6 link-local, ULA, link-local multicast
        QTest::newRow("fe80::/10") << "fe80::1" << true;
        QTest::newRow("fe80::/10 scoped") << "fe80::1c2b:3cff:fe4d:5e6f%en0" << true;
        QTest::newRow("febf (still fe80::/10)") << "febf::1" << true;
        QTest::newRow("ULA fc00::/7 low") << "fc00::1" << true;
        QTest::newRow("ULA fd00::/8") << "fd12:3456:789a::1" << true;
        QTest::newRow("all-nodes ff02::1") << "ff02::1" << true;
        QTest::newRow("all-nodes ff02::1 scoped") << "ff02::1%en0" << true;

        // Global, loopback and other IPv6
        QTest::newRow("global 2001:db8::1") << "2001:db8::1" << false;
        QTest::newRow("global 2a00:1450::1") << "2a00:1450:4001::1" << false;
        QTest::newRow("loopback v6") << "::1" << false;
        QTest::newRow("unspecified v6") << "::" << false;
        QTest::newRow("deprecated site-local fec0::/10") << "fec0::1" << false;
        QTest::newRow("site-local multicast ff05::1") << "ff05::1" << false;
        QTest::newRow("global multicast ff0e::1") << "ff0e::1" << false;
        QTest::newRow("just above ULA fe00::") << "fe00::1" << false;

        // IPv4-mapped IPv6 is judged by its IPv4 address
        QTest::newRow("mapped private") << "::ffff:192.168.1.5" << true;
        QTest::newRow("mapped public") << "::ffff:8.8.8.8" << false;
    }

    void wakeTargets()
    {
        QFETCH(QString, address);
        QFETCH(bool, permitted);

        QHostAddress parsed;
        QVERIFY2(parsed.setAddress(address), qPrintable(address));
        QCOMPARE(LanPolicy::isPermittedWakeTarget(parsed), permitted);
    }

    void nullAddressIsRefused()
    {
        QVERIFY(!LanPolicy::isPermittedWakeTarget(QHostAddress()));
    }

    void specialAddressEnums()
    {
        QVERIFY(LanPolicy::isPermittedWakeTarget(QHostAddress(QHostAddress::Broadcast)));
        QVERIFY(!LanPolicy::isPermittedWakeTarget(QHostAddress(QHostAddress::LocalHost)));
        QVERIFY(!LanPolicy::isPermittedWakeTarget(QHostAddress(QHostAddress::LocalHostIPv6)));
        QVERIFY(!LanPolicy::isPermittedWakeTarget(QHostAddress(QHostAddress::Any)));
        QVERIFY(!LanPolicy::isPermittedWakeTarget(QHostAddress(QHostAddress::AnyIPv4)));
        QVERIFY(!LanPolicy::isPermittedWakeTarget(QHostAddress(QHostAddress::AnyIPv6)));
    }
};

int runLanPolicyTests(int argc, char** argv)
{
    TestLanPolicy tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_lanpolicy.moc"
