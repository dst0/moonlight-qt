#pragma once

#include <QByteArray>
#include <QSslCertificate>
#include <QSslKey>

// Test-only X.509 certificates generated at run time with OpenSSL (RSA 2048).
struct TestCert
{
    QByteArray certPem;
    QByteArray keyPem;  // traditional ("BEGIN RSA PRIVATE KEY") PEM

    QSslCertificate cert() const { return QSslCertificate(certPem, QSsl::Pem); }
    QSslKey key() const { return QSslKey(keyPem, QSsl::Rsa, QSsl::Pem); }
};

namespace TestCerts
{

// Self-signed certificate without subjectAltName, like a Sunshine/GFE host certificate
TestCert makeSelfSigned(const char* commonName);

// Certificate authority (basicConstraints CA:TRUE)
TestCert makeCa(const char* commonName);

// Server certificate for IP 127.0.0.1 (subjectAltName) issued by ca
TestCert makeLocalhostServerCert(const TestCert& ca, const char* commonName);

}
