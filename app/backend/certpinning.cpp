#include "certpinning.h"

#include <QDebug>
#include <QNetworkReply>
#include <QSslEllipticCurve>
#include <QVector>

#define REJECTED_PROPERTY "certPinningRejected"

bool CertPinning::matchesPinned(const QSslCertificate& pinned, const QSslCertificate& presented)
{
    if (pinned.isNull() || presented.isNull()) {
        return false;
    }

    // QSslCertificate equality compares the DER encoding
    return pinned == presented;
}

bool CertPinning::errorsConcernOnlyPinned(const QSslCertificate& pinned, const QList<QSslError>& errors)
{
    if (pinned.isNull() || errors.isEmpty()) {
        return false;
    }

    for (const QSslError& error : errors) {
        if (!matchesPinned(pinned, error.certificate())) {
            return false;
        }
    }

    return true;
}

QSslConfiguration CertPinning::restrictToPinned(const QSslConfiguration& base, const QSslCertificate& pinned)
{
    QSslConfiguration config(base);

    // Replacing the CA list also disables on-demand loading of system root certificates
    config.setCaCertificates(pinned.isNull() ? QList<QSslCertificate>() : QList<QSslCertificate>{pinned});
    config.setProtocol(QSsl::TlsV1_3OrLater);
    config.setEllipticCurves(QVector<QSslEllipticCurve>());
    config.setBackendConfigurationOption("Groups", "X25519MLKEM768");
    return config;
}

void CertPinning::enforce(QNetworkReply* reply, const QSslCertificate& pinned)
{
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply, pinned](const QList<QSslError>& errors) {
        // Errors may also be ignored if the peer's leaf certificate is the pinned one but it
        // was sent with issuer certificates that are not trusted (only the pin is a CA).
        if (errorsConcernOnlyPinned(pinned, errors) ||
                (!errors.isEmpty() && matchesPinned(pinned, reply->sslConfiguration().peerCertificate()))) {
            reply->ignoreSslErrors(errors);
        }
        else {
            reply->setProperty(REJECTED_PROPERTY, true);
            for (const QSslError& error : errors) {
                qWarning() << "Rejecting TLS connection:" << error.errorString();
            }
        }
    });

    QObject::connect(reply, &QNetworkReply::encrypted, reply, [reply, pinned]() {
        if (!matchesPinned(pinned, reply->sslConfiguration().peerCertificate())) {
            qWarning() << "Host certificate does not match the pinned certificate; aborting request to"
                       << reply->url().host();
            reply->setProperty(REJECTED_PROPERTY, true);
            reply->abort();
        }
    });
}

bool CertPinning::wasRejected(const QNetworkReply* reply)
{
    return reply->property(REJECTED_PROPERTY).toBool();
}
