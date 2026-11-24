#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include <stdexcept>

#include "backend/identitymanager.h"
#include "backend/keychain_mac.h"
#include "testcerts.h"
#include "tests.h"

namespace {

// Test-only Keychain service names; never the production service
QString testService(const char* suffix)
{
    return QStringLiteral("com.moonlight-stream.Moonlight.identity.unittest.%1.%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QString::fromLatin1(suffix));
}

const QString kAccount = QString::fromLatin1(Keychain::ClientKeyAccount);

void removeItem(const QString& service)
{
    QString error;
    Keychain::Status status = Keychain::remove(service, kAccount, &error);
    QVERIFY2(status != Keychain::Status::Error, qPrintable(error));
}

QByteArray readItem(const QString& service, Keychain::Status* statusOut = nullptr)
{
    QByteArray data;
    QString error;
    Keychain::Status status = Keychain::read(service, kAccount, data, &error);
    if (statusOut != nullptr) {
        *statusOut = status;
    }
    if (status == Keychain::Status::Error) {
        qWarning() << error;
    }
    return data;
}

void writeItem(const QString& service, const QByteArray& data)
{
    QString error;
    QCOMPARE(Keychain::write(service, kAccount, data, &error), Keychain::Status::Ok);
}

}

// Keychain wrapper (Security.framework SecItem API)
class TestKeychain : public QObject
{
    Q_OBJECT

    const QString m_Service = testService("wrapper");
    const QString m_OtherService = testService("wrapper-other");

private slots:
    void init()
    {
        removeItem(m_Service);
        removeItem(m_OtherService);
    }

    void cleanup()
    {
        removeItem(m_Service);
        removeItem(m_OtherService);
    }

    void productionNamesAreStable()
    {
        QCOMPARE(QString::fromLatin1(Keychain::IdentityService),
                 QStringLiteral("local.secure-desktop-stream.Moonlight.identity"));
        QCOMPARE(kAccount, QStringLiteral("client-key"));
        QVERIFY(m_Service != QString::fromLatin1(Keychain::IdentityService));
    }

    void profilesUseStableSeparateKeychainIdentities()
    {
        const QString a = Keychain::identityServiceForProfile(QStringLiteral("/tmp/sds/a.ini"));
        const QString b = Keychain::identityServiceForProfile(QStringLiteral("/tmp/sds/b.ini"));
        QVERIFY(a != b);
        QVERIFY(a.startsWith(QString::fromLatin1(Keychain::IdentityService) + QLatin1Char('.')));
        QCOMPARE(a, Keychain::identityServiceForProfile(QStringLiteral("/tmp/sds/../sds/a.ini")));
        QVERIFY(!a.contains(QStringLiteral("/tmp/")));
        QVERIFY(Keychain::identityServiceForProfile(QString()).isEmpty());
    }

    void readMissingItemIsNotFound()
    {
        Keychain::Status status;
        QByteArray data = readItem(m_Service, &status);
        QCOMPARE(status, Keychain::Status::NotFound);
        QVERIFY(data.isEmpty());
    }

    void writeThenRead()
    {
        writeItem(m_Service, "first value");
        Keychain::Status status;
        QCOMPARE(readItem(m_Service, &status), QByteArray("first value"));
        QCOMPARE(status, Keychain::Status::Ok);
    }

    void binaryAndLargeData()
    {
        QByteArray binary("a\0b\0\xff\x01", 6);
        binary += QByteArray(16 * 1024, 'k');
        writeItem(m_Service, binary);
        QCOMPARE(readItem(m_Service), binary);
    }

    void writeReplacesExistingItem()
    {
        writeItem(m_Service, "old");
        writeItem(m_Service, "new");
        QCOMPARE(readItem(m_Service), QByteArray("new"));
    }

    void removeDeletesItem()
    {
        writeItem(m_Service, "value");
        QString error;
        QCOMPARE(Keychain::remove(m_Service, kAccount, &error), Keychain::Status::Ok);
        Keychain::Status status;
        readItem(m_Service, &status);
        QCOMPARE(status, Keychain::Status::NotFound);
        QCOMPARE(Keychain::remove(m_Service, kAccount, &error), Keychain::Status::NotFound);
    }

    void servicesAndAccountsAreIsolated()
    {
        writeItem(m_Service, "value");
        Keychain::Status status;
        readItem(m_OtherService, &status);
        QCOMPARE(status, Keychain::Status::NotFound);

        QByteArray data;
        QCOMPARE(Keychain::read(m_Service, QStringLiteral("another-account"), data), Keychain::Status::NotFound);
    }
};

// IdentityManager key storage, migration and recovery on macOS
class TestIdentityKeyStorage : public QObject
{
    Q_OBJECT

    const QString m_Service = testService("identity");
    QTemporaryDir* m_Dir = nullptr;

    QString settingsPath() const { return m_Dir->filePath(QStringLiteral("Moonlight.ini")); }

    // The raw settings file must never contain a private key once IdentityManager is done
    void verifySettingsFileHasNoPrivateKey()
    {
        QFile file(settingsPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray contents = file.readAll();
        QVERIFY2(!contents.contains("PRIVATE KEY"), contents.constData());
    }

    struct Identity
    {
        QByteArray cert;
        QByteArray key;
        QString uniqueId;
    };

    // Creates an independent identity (its own settings and Keychain item) and returns it
    Identity makeForeignIdentity()
    {
        QTemporaryDir dir;
        QString service = testService("foreign");
        removeItem(service);
        Identity identity;
        {
            QSettings settings(dir.filePath(QStringLiteral("foreign.ini")), QSettings::IniFormat);
            IdentityManager im(settings, service);
            identity = {im.getCertificate(), im.getPrivateKey(), im.getUniqueId()};
        }
        removeItem(service);
        return identity;
    }

private slots:
    void init()
    {
        delete m_Dir;
        m_Dir = new QTemporaryDir();
        QVERIFY(m_Dir->isValid());
        removeItem(m_Service);
    }

    void cleanup()
    {
        removeItem(m_Service);
        delete m_Dir;
        m_Dir = nullptr;
    }

    void freshInstallStoresKeyOnlyInKeychain()
    {
        QByteArray cert, key;
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            IdentityManager im(settings, m_Service);
            cert = im.getCertificate();
            key = im.getPrivateKey();

            QVERIFY(!cert.isEmpty());
            QVERIFY(key.contains("PRIVATE KEY"));
            QVERIFY(IdentityManager::isMatchingKeyPair(cert, key));
            QVERIFY(!im.getSslConfig().privateKey().isNull());
            QVERIFY(!im.getUniqueId().isEmpty());

            QCOMPARE(settings.value("certificate").toByteArray(), cert);
            QVERIFY(!settings.contains("key"));
        }

        QCOMPARE(readItem(m_Service), key);
        verifySettingsFileHasNoPrivateKey();
    }

    void restartLoadsTheSameIdentity()
    {
        Identity first;
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            IdentityManager im(settings, m_Service);
            first = {im.getCertificate(), im.getPrivateKey(), im.getUniqueId()};
        }
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            IdentityManager im(settings, m_Service);
            QCOMPARE(im.getCertificate(), first.cert);
            QCOMPARE(im.getPrivateKey(), first.key);
            QCOMPARE(im.getUniqueId(), first.uniqueId);
        }
        verifySettingsFileHasNoPrivateKey();
    }

    void migratesPlaintextKeyFromSettings()
    {
        // Settings as written by an older client: certificate and plaintext key, no Keychain item
        Identity legacy = makeForeignIdentity();
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            settings.setValue("certificate", legacy.cert);
            settings.setValue("key", legacy.key);
            settings.setValue("uniqueid", legacy.uniqueId);
        }

        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            IdentityManager im(settings, m_Service);

            // Same identity (no regeneration, so pairings survive)
            QCOMPARE(im.getCertificate(), legacy.cert);
            QCOMPARE(im.getPrivateKey(), legacy.key);
            QCOMPARE(im.getUniqueId(), legacy.uniqueId);

            QVERIFY(!settings.contains("key"));
        }

        QCOMPARE(readItem(m_Service), legacy.key);
        verifySettingsFileHasNoPrivateKey();
    }

    void removesLeftoverPlaintextKeyWhenKeychainHasIt()
    {
        Identity identity = makeForeignIdentity();
        writeItem(m_Service, identity.key);
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            settings.setValue("certificate", identity.cert);
            settings.setValue("key", identity.key);
        }

        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            IdentityManager im(settings, m_Service);
            QCOMPARE(im.getCertificate(), identity.cert);
            QCOMPARE(im.getPrivateKey(), identity.key);
            QVERIFY(!settings.contains("key"));
        }

        QCOMPARE(readItem(m_Service), identity.key);
        verifySettingsFileHasNoPrivateKey();
    }

    void matchingPlaintextKeyReplacesMismatchedKeychainKey()
    {
        // The Keychain holds the key of another identity, settings hold a matching pair
        // (e.g. written by an unmodified client after the migration)
        Identity stale = makeForeignIdentity();
        Identity current = makeForeignIdentity();
        writeItem(m_Service, stale.key);
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            settings.setValue("certificate", current.cert);
            settings.setValue("key", current.key);
        }

        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            IdentityManager im(settings, m_Service);
            QCOMPARE(im.getCertificate(), current.cert);
            QCOMPARE(im.getPrivateKey(), current.key);
        }

        QCOMPARE(readItem(m_Service), current.key);
        verifySettingsFileHasNoPrivateKey();
    }

    void missingMatchingKeyPreservesTheExistingPairingIdentity()
    {
        Identity identity = makeForeignIdentity();
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            settings.setValue("certificate", identity.cert);
            settings.setValue("key", QByteArray("-----BEGIN " "PRIVATE KEY-----\ngarbage\n-----END " "PRIVATE KEY-----\n"));
        }

        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            QVERIFY_EXCEPTION_THROWN(IdentityManager(settings, m_Service), std::runtime_error);
            QCOMPARE(settings.value("certificate").toByteArray(), identity.cert);
        }
    }

    void keychainKeyWithoutCertificateIsPreservedForRecovery()
    {
        Identity orphan = makeForeignIdentity();
        writeItem(m_Service, orphan.key);

        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            QVERIFY_EXCEPTION_THROWN(IdentityManager(settings, m_Service), std::runtime_error);
            QVERIFY(!settings.contains("certificate"));
        }

        QCOMPARE(readItem(m_Service), orphan.key);
        QVERIFY(!QFile::exists(settingsPath()));
    }

    void isMatchingKeyPair()
    {
        Identity a = makeForeignIdentity();
        Identity b = makeForeignIdentity();
        TestCert selfSigned = TestCerts::makeSelfSigned("Key pair test");

        QVERIFY(IdentityManager::isMatchingKeyPair(a.cert, a.key));
        QVERIFY(IdentityManager::isMatchingKeyPair(selfSigned.certPem, selfSigned.keyPem));
        QVERIFY(!IdentityManager::isMatchingKeyPair(a.cert, b.key));
        QVERIFY(!IdentityManager::isMatchingKeyPair(b.cert, a.key));
        QVERIFY(!IdentityManager::isMatchingKeyPair(a.cert, QByteArray()));
        QVERIFY(!IdentityManager::isMatchingKeyPair(QByteArray(), a.key));
        QVERIFY(!IdentityManager::isMatchingKeyPair(a.cert, QByteArray("not a key")));
        QVERIFY(!IdentityManager::isMatchingKeyPair(QByteArray("not a cert"), a.key));
    }
};

int runKeychainTests(int argc, char** argv)
{
    int status = 0;
    {
        TestKeychain tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        TestIdentityKeyStorage tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    return status;
}

#include "tst_keychain.moc"
