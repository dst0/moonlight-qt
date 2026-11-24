#pragma once

#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSettings>

class IdentityManager
{
public:
    QString
    getUniqueId();

    QByteArray
    getCertificate();

    QByteArray
    getPrivateKey();

    QSslConfiguration
    getSslConfig();

    static
    IdentityManager*
    get();

    // Loads (or creates) the identity from settings. On macOS the private key lives in the
    // Keychain item (keychainService, "client-key") and a plaintext key found in settings is
    // migrated there and removed from settings; Keychain failures are fatal. keychainService
    // is ignored on other platforms. Only tests construct this directly; use get() otherwise.
    IdentityManager(QSettings& settings, const QString& keychainService);
    ~IdentityManager();

    // True if keyPem is a PEM private key that belongs to the PEM certificate certPem
    static
    bool
    isMatchingKeyPair(const QByteArray& certPem, const QByteArray& keyPem);

private:

    QSslCertificate
    getSslCertificate();

    QSslKey
    getSslKey();

    void
    createCredentials(QSettings& settings);

#ifdef Q_OS_DARWIN
    QByteArray
    loadKeychainPrivateKey(QSettings& settings);

    void
    storeKeychainPrivateKey(const QByteArray& key);

    static
    QByteArray
    readPlaintextKey(QSettings& settings);

    static
    void
    removePlaintextKey(QSettings& settings);

    QString m_KeychainService;
#endif

    // Initialized in constructor
    QByteArray m_CachedPrivateKey;
    QByteArray m_CachedPemCert;

    // Lazy initialized
    QString m_CachedUniqueId;
    QSslCertificate m_CachedSslCert;
    QSslKey m_CachedSslKey;

    static IdentityManager* s_Im;
};
