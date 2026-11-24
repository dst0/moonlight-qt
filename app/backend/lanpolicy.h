#pragma once

#include <QHostAddress>

// Address policy for the LAN-only client.
namespace LanPolicy
{

// Returns true if a Wake-on-LAN packet may be sent to this address:
//  - IPv4: RFC 1918 private ranges (10/8, 172.16/12, 192.168/16), link-local
//    (169.254/16) and the limited broadcast address 255.255.255.255.
//    IPv4-mapped IPv6 addresses are judged by their IPv4 address.
//  - IPv6: link-local (fe80::/10), unique local (fc00::/7) and link-local
//    scope multicast (ff02::/16, e.g. the all-nodes address ff02::1).
// Everything else (public addresses, loopback, CGNAT, other multicast scopes,
// the null address) is refused.
bool isPermittedWakeTarget(const QHostAddress& address);

// Connections accept literal private/link-local addresses and loopback for SSH tunnels.
// Host names, public addresses, multicast and broadcast destinations are refused before DNS.
bool isPermittedConnectionTarget(const QHostAddress& address);

}
