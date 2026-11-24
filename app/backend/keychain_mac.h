#pragma once

#include <QByteArray>
#include <QString>

// Minimal synchronous wrapper around the macOS Keychain (Security.framework SecItem API)
// for generic password items. macOS only.
namespace Keychain
{

enum class Status
{
    Ok,
    NotFound,
    Error,
};

// Service and account of the generic password item holding the client private key (PEM)
extern const char* const IdentityService;   // private fork namespace; never shared with upstream
extern const char* const ClientKeyAccount;  // "client-key"

// Bind a Keychain identity to one settings profile, including isolated portable profiles.
QString identityServiceForProfile(const QString& settingsPath);

// Reads the item's data. Returns NotFound if no such item exists.
Status read(const QString& service, const QString& account, QByteArray& data, QString* error = nullptr);

// Creates the item, or replaces the data of an existing one, requesting
// kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly. Note: that attribute is only enforced by
// the data protection keychain, which requires a provisioned signing identity. Ad-hoc or
// unsigned builds store the item in the file-based login keychain, where it is protected by
// the keychain lock and the item's access list (which trusts the creating app's signature).
Status write(const QString& service, const QString& account, const QByteArray& data, QString* error = nullptr);

// Deletes the item. Returns NotFound if no such item exists.
Status remove(const QString& service, const QString& account, QString* error = nullptr);

}
