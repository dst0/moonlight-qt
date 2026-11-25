#include "identitymanager.h"
#include "utils.h"

#ifdef Q_OS_DARWIN
#include "keychain_mac.h"
#endif

#include <QDebug>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>

#define SER_UNIQUEID "uniqueid"
#define SER_CERT "certificate"
#define SER_KEY "key"

IdentityManager* IdentityManager::s_Im = nullptr;

IdentityManager*
IdentityManager::get()
{
    // This will always be called first on the main thread,
    // so it's safe to initialize without locks.
    if (s_Im == nullptr) {
        QSettings settings;
#ifdef Q_OS_DARWIN
        s_Im = new IdentityManager(settings, Keychain::identityServiceForProfile(settings.fileName()));
#else
        s_Im = new IdentityManager(settings, QString());
#endif
    }

    return s_Im;
}

void IdentityManager::createCredentials(QSettings& settings)
{
    X509* cert = X509_new();
    THROW_BAD_ALLOC_IF_NULL(cert);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY* pk = EVP_RSA_gen(2048);
    THROW_BAD_ALLOC_IF_NULL(pk);
#else
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    THROW_BAD_ALLOC_IF_NULL(ctx);

    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);

    // pk must be initialized on input
    EVP_PKEY* pk = NULL;
    EVP_PKEY_keygen(ctx, &pk);

    EVP_PKEY_CTX_free(ctx);
    THROW_BAD_ALLOC_IF_NULL(pk);
#endif

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 0);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#else
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#endif

    X509_set_pubkey(cert, pk);

    X509_NAME* name = X509_NAME_new();
    THROW_BAD_ALLOC_IF_NULL(name);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<unsigned char *>(const_cast<char*>("NVIDIA GameStream Client")),
                               -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    X509_sign(cert, pk, EVP_sha256());

    BIO* biokey = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biokey);
    PEM_write_bio_PrivateKey(biokey, pk, NULL, NULL, 0, NULL, NULL);

    BIO* biocert = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biocert);
    PEM_write_bio_X509(biocert, cert);

    BUF_MEM* mem;
    BIO_get_mem_ptr(biokey, &mem);
    m_CachedPrivateKey = QByteArray(mem->data, (int)mem->length);
    m_CachedSslKey = QSslKey();
    OPENSSL_cleanse(mem->data, mem->length);

    BIO_get_mem_ptr(biocert, &mem);
    m_CachedPemCert = QByteArray(mem->data, (int)mem->length);
    m_CachedSslCert = QSslCertificate();

    X509_free(cert);
    EVP_PKEY_free(pk);
    BIO_free(biokey);
    BIO_free(biocert);

    // Check that the new keypair is valid before persisting it
    if (getSslCertificate().isNull()) {
        qFatal("Newly generated certificate is unreadable");
    }
    if (getSslKey().isNull()) {
        qFatal("Newly generated private key is unreadable");
    }

#ifdef Q_OS_DARWIN
    // The private key is persisted only in the Keychain. Store it before the certificate:
    // if we are interrupted in between, the old certificate no longer matches the new key
    // and the next start either migrates the old plaintext key or regenerates.
    storeKeychainPrivateKey(m_CachedPrivateKey);
    settings.setValue(SER_CERT, m_CachedPemCert);
    removePlaintextKey(settings);

    qInfo() << "Wrote new identity credentials (private key in the macOS Keychain)";
#else
    settings.setValue(SER_CERT, m_CachedPemCert);
    settings.setValue(SER_KEY, m_CachedPrivateKey);

    qInfo() << "Wrote new identity credentials to settings";
#endif
}

bool IdentityManager::isMatchingKeyPair(const QByteArray& certPem, const QByteArray& keyPem)
{
    if (certPem.isEmpty() || keyPem.isEmpty()) {
        return false;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    BIO* bio = BIO_new_mem_buf(const_cast<char*>(certPem.constData()), certPem.size());
#else
    BIO* bio = BIO_new_mem_buf(certPem.constData(), certPem.size());
#endif
    THROW_BAD_ALLOC_IF_NULL(bio);
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        return false;
    }

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    bio = BIO_new_mem_buf(const_cast<char*>(keyPem.constData()), keyPem.size());
#else
    bio = BIO_new_mem_buf(keyPem.constData(), keyPem.size());
#endif
    THROW_BAD_ALLOC_IF_NULL(bio);
    EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (pk == nullptr) {
        X509_free(cert);
        return false;
    }

    bool matches = X509_check_private_key(cert, pk) == 1;

    EVP_PKEY_free(pk);
    X509_free(cert);
    return matches;
}

#ifdef Q_OS_DARWIN
QByteArray IdentityManager::loadKeychainPrivateKey(QSettings& settings)
{
    QByteArray keychainKey;
    QString error;
    Keychain::Status status = Keychain::read(m_KeychainService, Keychain::ClientKeyAccount, keychainKey, &error);
    if (status == Keychain::Status::Error) {
        // Never fall back to a plaintext key or silently regenerate (which would unpair all hosts)
        qFatal("Unable to read the client private key from the macOS Keychain: %s", qPrintable(error));
    }

    QByteArray plaintextKey = readPlaintextKey(settings);

    if (status == Keychain::Status::Ok && isMatchingKeyPair(m_CachedPemCert, keychainKey)) {
        if (!plaintextKey.isEmpty()) {
            // Leftover from an interrupted migration or an older client version
            qInfo() << "Removing a plaintext private key from settings; the Keychain copy is used";
            removePlaintextKey(settings);
        }
        plaintextKey.fill('\0');
        return keychainKey;
    }

    if (isMatchingKeyPair(m_CachedPemCert, plaintextKey)) {
        qInfo() << "Migrating the client private key from settings to the macOS Keychain";
        storeKeychainPrivateKey(plaintextKey);
        removePlaintextKey(settings);
        keychainKey.fill('\0');
        return plaintextKey;
    }

    if (status == Keychain::Status::Ok) {
        qWarning() << "The private key in the macOS Keychain does not match the certificate";
    }
    if (!plaintextKey.isEmpty()) {
        qWarning() << "The plaintext private key in settings does not match the certificate";
    }
    const bool hasAnyIdentity = !m_CachedPemCert.isEmpty() || status == Keychain::Status::Ok || !plaintextKey.isEmpty();
    keychainKey.fill('\0');
    plaintextKey.fill('\0');
    if (hasAnyIdentity) {
        throw std::runtime_error("The pairing certificate or key for this profile is missing or does not match. "
                                 "Restore the matching certificate and Keychain item or use a new profile. "
                                 "Existing pairings have been preserved.");
    }

    // No usable key; new credentials will be generated (and the plaintext key removed)
    return QByteArray();
}

void IdentityManager::storeKeychainPrivateKey(const QByteArray& key)
{
    QString error;
    if (Keychain::write(m_KeychainService, Keychain::ClientKeyAccount, key, &error) != Keychain::Status::Ok) {
        qFatal("Unable to store the client private key in the macOS Keychain: %s", qPrintable(error));
    }

    // Read it back to be sure the Keychain really holds this key before the
    // plaintext copy is deleted
    QByteArray readBack;
    if (Keychain::read(m_KeychainService, Keychain::ClientKeyAccount, readBack, &error) != Keychain::Status::Ok) {
        readBack.fill('\0');
        qFatal("Unable to read back the client private key from the macOS Keychain: %s", qPrintable(error));
    }
    const bool matches = (readBack == key);
    readBack.fill('\0');
    if (!matches) {
        qFatal("The client private key read back from the macOS Keychain differs from the stored key");
    }
}

// Only Moonlight's own settings location is considered: fallback locations (organization
// or system-wide defaults) are never written by Moonlight and cannot be cleaned up by it.
QByteArray IdentityManager::readPlaintextKey(QSettings& settings)
{
    bool fallbacksEnabled = settings.fallbacksEnabled();
    settings.setFallbacksEnabled(false);
    QByteArray key = settings.value(SER_KEY).toByteArray();
    settings.setFallbacksEnabled(fallbacksEnabled);
    return key;
}

void IdentityManager::removePlaintextKey(QSettings& settings)
{
    bool fallbacksEnabled = settings.fallbacksEnabled();
    settings.setFallbacksEnabled(false);
    settings.remove(SER_KEY);
    settings.sync();
    bool removed = settings.status() == QSettings::NoError && !settings.contains(SER_KEY);
    settings.setFallbacksEnabled(fallbacksEnabled);

    if (!removed) {
        qFatal("Unable to remove the plaintext client private key from settings (status %d)", (int)settings.status());
    }
}
#endif

IdentityManager::IdentityManager(QSettings& settings, const QString& keychainService)
#ifdef Q_OS_DARWIN
    : m_KeychainService(keychainService)
#endif
{
    m_CachedPemCert = settings.value(SER_CERT).toByteArray();
#ifdef Q_OS_DARWIN
    Q_ASSERT(!m_KeychainService.isEmpty());
    m_CachedPrivateKey = loadKeychainPrivateKey(settings);
#else
    Q_UNUSED(keychainService);
    m_CachedPrivateKey = settings.value(SER_KEY).toByteArray();
#endif

    if (m_CachedPemCert.isEmpty() || m_CachedPrivateKey.isEmpty()) {
        qInfo() << "No existing credentials found";
        createCredentials(settings);
    }
    else if (getSslCertificate().isNull()) {
        qWarning() << "Certificate is unreadable";
        createCredentials(settings);
    }
    else if (getSslKey().isNull()) {
        qWarning() << "Private key is unreadable";
        createCredentials(settings);
    }

    // We should have valid credentials now. If not, we're screwed
    if (getSslCertificate().isNull()) {
        qFatal("Certificate is unreadable");
    }
    if (getSslKey().isNull()) {
        qFatal("Private key is unreadable");
    }

    // Load the unique ID from settings
    m_CachedUniqueId = settings.value(SER_UNIQUEID).toString();
    if (!m_CachedUniqueId.isEmpty()) {
        qInfo() << "Loaded unique ID from settings:" << m_CachedUniqueId;
    }
    else {
        // Generate a new unique ID in base 16
        uint64_t uid;
        RAND_bytes(reinterpret_cast<unsigned char*>(&uid), sizeof(uid));
        m_CachedUniqueId = QString::number(uid, 16);

        qInfo() << "Generated new unique ID:" << m_CachedUniqueId;

        settings.setValue(SER_UNIQUEID, m_CachedUniqueId);
    }
}

IdentityManager::~IdentityManager()
{
    if (!m_CachedPrivateKey.isEmpty()) {
        m_CachedPrivateKey.fill('\0');
    }
}

QSslCertificate
IdentityManager::getSslCertificate()
{
    if (m_CachedSslCert.isNull()) {
        m_CachedSslCert = QSslCertificate(m_CachedPemCert);
    }
    return m_CachedSslCert;
}

QSslKey
IdentityManager::getSslKey()
{
    if (m_CachedSslKey.isNull()) {
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
        BIO* bio = BIO_new_mem_buf(const_cast<char*>(m_CachedPrivateKey.constData()), -1);
#else
        BIO* bio = BIO_new_mem_buf(m_CachedPrivateKey.constData(), -1);
#endif
        THROW_BAD_ALLOC_IF_NULL(bio);

        EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (pk == nullptr) {
            return m_CachedSslKey;
        }

        bio = BIO_new(BIO_s_mem());
        THROW_BAD_ALLOC_IF_NULL(bio);

        // We must write out our PEM in the old PKCS1 format for SecureTransport
        // on macOS/iOS to be able to read it.
#ifdef Q_OS_DARWIN
        PEM_write_bio_PrivateKey_traditional(bio, pk, nullptr, nullptr, 0, nullptr, 0);
#else
        PEM_write_bio_PrivateKey(bio, pk, nullptr, nullptr, 0, nullptr, 0);
#endif

        BUF_MEM* mem;
        BIO_get_mem_ptr(bio, &mem);
        QByteArray pkcs1Pem(mem->data, (int)mem->length);
        m_CachedSslKey = QSslKey(pkcs1Pem, getSslCertificate().publicKey().algorithm());
        pkcs1Pem.fill('\0');
        OPENSSL_cleanse(mem->data, mem->length);

        BIO_free(bio);
        EVP_PKEY_free(pk);
    }
    return m_CachedSslKey;
}

QSslConfiguration
IdentityManager::getSslConfig()
{
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(getSslCertificate());
    sslConfig.setPrivateKey(getSslKey());
    return sslConfig;
}

QString
IdentityManager::getUniqueId()
{
    return m_CachedUniqueId;
}

QByteArray
IdentityManager::getCertificate()
{
    return m_CachedPemCert;
}

QByteArray
IdentityManager::getPrivateKey()
{
    return m_CachedPrivateKey;
}
