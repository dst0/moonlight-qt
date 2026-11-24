#include "testcerts.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdexcept>

namespace {

void check(bool ok, const char* what)
{
    if (!ok) {
        throw std::runtime_error(what);
    }
}

QByteArray toPem(X509* cert)
{
    BIO* bio = BIO_new(BIO_s_mem());
    check(bio != nullptr && PEM_write_bio_X509(bio, cert) == 1, "PEM_write_bio_X509");
    BUF_MEM* mem;
    BIO_get_mem_ptr(bio, &mem);
    QByteArray pem(mem->data, (int)mem->length);
    BIO_free(bio);
    return pem;
}

QByteArray toPem(EVP_PKEY* key)
{
    BIO* bio = BIO_new(BIO_s_mem());
    check(bio != nullptr &&
          PEM_write_bio_PrivateKey_traditional(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1,
          "PEM_write_bio_PrivateKey_traditional");
    BUF_MEM* mem;
    BIO_get_mem_ptr(bio, &mem);
    QByteArray pem(mem->data, (int)mem->length);
    BIO_free(bio);
    return pem;
}

void addExtension(X509* cert, X509* issuer, int nid, const char* value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    check(ext != nullptr, "X509V3_EXT_conf_nid");
    check(X509_add_ext(cert, ext, -1) == 1, "X509_add_ext");
    X509_EXTENSION_free(ext);
}

// Creates an unsigned certificate for key issued by issuer (self-issued if issuer is null)
X509* makeCert(EVP_PKEY* key, const char* commonName, X509* issuer)
{
    X509* cert = X509_new();
    check(cert != nullptr, "X509_new");
    X509_set_version(cert, 2);

    unsigned char serial[8];
    check(RAND_bytes(serial, sizeof(serial)) == 1, "RAND_bytes");
    serial[0] &= 0x7F;
    BIGNUM* bn = BN_bin2bn(serial, sizeof(serial), nullptr);
    check(bn != nullptr && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert)) != nullptr, "serial");
    BN_free(bn);

    X509_gmtime_adj(X509_getm_notBefore(cert), -60 * 60);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24);
    check(X509_set_pubkey(cert, key) == 1, "X509_set_pubkey");

    X509_NAME* name = X509_NAME_new();
    check(name != nullptr, "X509_NAME_new");
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0);
    check(X509_set_subject_name(cert, name) == 1, "subject");
    check(X509_set_issuer_name(cert, issuer ? X509_get_subject_name(issuer) : name) == 1, "issuer");
    X509_NAME_free(name);
    return cert;
}

void sign(X509* cert, EVP_PKEY* key)
{
    check(X509_sign(cert, key, EVP_sha256()) > 0, "X509_sign");
}

EVP_PKEY* parseKey(const QByteArray& pem)
{
    BIO* bio = BIO_new_mem_buf(pem.constData(), pem.size());
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    check(key != nullptr, "PEM_read_bio_PrivateKey");
    return key;
}

X509* parseCert(const QByteArray& pem)
{
    BIO* bio = BIO_new_mem_buf(pem.constData(), pem.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    check(cert != nullptr, "PEM_read_bio_X509");
    return cert;
}

TestCert finish(X509* cert, EVP_PKEY* key)
{
    TestCert result{toPem(cert), toPem(key)};
    X509_free(cert);
    EVP_PKEY_free(key);
    return result;
}

}

TestCert TestCerts::makeSelfSigned(const char* commonName)
{
    EVP_PKEY* key = EVP_RSA_gen(2048);
    check(key != nullptr, "EVP_RSA_gen");
    X509* cert = makeCert(key, commonName, nullptr);
    sign(cert, key);
    return finish(cert, key);
}

TestCert TestCerts::makeCa(const char* commonName)
{
    EVP_PKEY* key = EVP_RSA_gen(2048);
    check(key != nullptr, "EVP_RSA_gen");
    X509* cert = makeCert(key, commonName, nullptr);
    addExtension(cert, cert, NID_basic_constraints, "critical,CA:TRUE");
    addExtension(cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign");
    addExtension(cert, cert, NID_subject_key_identifier, "hash");
    sign(cert, key);
    return finish(cert, key);
}

TestCert TestCerts::makeLocalhostServerCert(const TestCert& ca, const char* commonName)
{
    EVP_PKEY* caKey = parseKey(ca.keyPem);
    X509* caCert = parseCert(ca.certPem);

    EVP_PKEY* key = EVP_RSA_gen(2048);
    check(key != nullptr, "EVP_RSA_gen");
    X509* cert = makeCert(key, commonName, caCert);
    addExtension(cert, caCert, NID_basic_constraints, "CA:FALSE");
    addExtension(cert, caCert, NID_key_usage, "critical,digitalSignature,keyEncipherment");
    addExtension(cert, caCert, NID_ext_key_usage, "serverAuth");
    addExtension(cert, caCert, NID_subject_alt_name, "IP:127.0.0.1");
    addExtension(cert, caCert, NID_authority_key_identifier, "keyid:always");
    sign(cert, caKey);

    X509_free(caCert);
    EVP_PKEY_free(caKey);
    return finish(cert, key);
}
