#include "lanpolicy.h"

bool LanPolicy::isPermittedConnectionTarget(const QHostAddress& address)
{
    bool ipv4 = false;
    const quint32 v4 = address.toIPv4Address(&ipv4);
    const QHostAddress normalized = ipv4 ? QHostAddress(v4) : address;
    return !normalized.isNull() && !normalized.isMulticast() &&
           normalized != QHostAddress(QHostAddress::Broadcast) &&
           (normalized.isLoopback() || isPermittedWakeTarget(normalized));
}

bool LanPolicy::isPermittedWakeTarget(const QHostAddress& address)
{
    bool isIpv4 = false;
    quint32 ipv4 = address.toIPv4Address(&isIpv4);
    if (isIpv4) {
        QHostAddress v4(ipv4);
        return v4.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
               v4.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
               v4.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16) ||
               v4.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16) ||
               v4 == QHostAddress(QHostAddress::Broadcast);
    }

    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        return address.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10) ||
               address.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7) ||
               address.isInSubnet(QHostAddress(QStringLiteral("ff02::")), 16);
    }

    return false;
}
