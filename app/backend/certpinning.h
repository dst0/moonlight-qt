#pragma once

#include <QList>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>

class QNetworkReply;

// Strict pinning of the host's TLS certificate for every HTTPS request.
//
// The pinned certificate is the one learned during pairing. A request succeeds only if
// the host presents exactly that certificate; a certificate that chains to a system
// root CA (and would therefore raise no TLS error) is rejected as well.
namespace CertPinning
{

// True only if both certificates are non-null and identical.
bool matchesPinned(const QSslCertificate& pinned, const QSslCertificate& presented);

// True if the errors may be ignored because every one of them concerns the pinned
// certificate itself (e.g. it is self-signed or does not name the host's IP address).
// False for a null pin, an empty error list, or any error about a different
// certificate or about no certificate at all.
bool errorsConcernOnlyPinned(const QSslCertificate& pinned, const QList<QSslError>& errors);

// Returns a copy of base whose CA database holds only the pinned certificate, so no
// other certificate can validate, even one issued by a system-trusted CA.
QSslConfiguration restrictToPinned(const QSslConfiguration& base, const QSslCertificate& pinned);

// Enforces the pin on a reply. Must be called right after QNetworkAccessManager::get()
// (before control returns to the event loop):
//  - sslErrors: ignored only if errorsConcernOnlyPinned() or the peer's leaf certificate
//    matchesPinned() (a pinned leaf sent with its issuer chain); otherwise the handshake fails;
//  - encrypted: the reply is aborted, before any request data is sent, unless the
//    peer certificate matchesPinned(). wasRejected() then returns true.
void enforce(QNetworkReply* reply, const QSslCertificate& pinned);

// True if enforce() aborted the reply because the peer certificate did not match.
bool wasRejected(const QNetworkReply* reply);

}
