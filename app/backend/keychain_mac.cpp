#include "keychain_mac.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

const char* const Keychain::IdentityService = "local.secure-desktop-stream.Moonlight.identity";
const char* const Keychain::ClientKeyAccount = "client-key";

QString Keychain::identityServiceForProfile(const QString& settingsPath)
{
    if (settingsPath.isEmpty()) {
        return QString();
    }
    const QString path = QDir::cleanPath(QFileInfo(settingsPath).absoluteFilePath());
    const QByteArray digest = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(IdentityService) + QLatin1Char('.') + QString::fromLatin1(digest);
}

namespace {

// Owns a CoreFoundation reference
template <typename T>
class CFRef
{
public:
    explicit CFRef(T ref = nullptr) : m_Ref(ref) {}
    ~CFRef() { if (m_Ref != nullptr) CFRelease(m_Ref); }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    T get() const { return m_Ref; }
    T* receive() { return &m_Ref; }

private:
    T m_Ref;
};

CFStringRef createCFString(const QString& string)
{
    QByteArray utf8 = string.toUtf8();
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(utf8.constData()),
                                   utf8.size(), kCFStringEncodingUTF8, false);
}

QString describeStatus(const char* operation, OSStatus status)
{
    QString message = QString("%1 failed with OSStatus %2").arg(operation).arg(status);

    CFRef<CFStringRef> description(SecCopyErrorMessageString(status, nullptr));
    if (description.get() != nullptr) {
        CFIndex length = CFStringGetLength(description.get());
        CFIndex bufferSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
        QByteArray buffer(bufferSize, 0);
        if (CFStringGetCString(description.get(), buffer.data(), bufferSize, kCFStringEncodingUTF8)) {
            message += ": " + QString::fromUtf8(buffer.constData());
        }
    }

    return message;
}

Keychain::Status fail(const char* operation, OSStatus status, QString* error)
{
    if (error != nullptr) {
        *error = describeStatus(operation, status);
    }
    return Keychain::Status::Error;
}

// Creates a query dictionary identifying the generic password item
CFMutableDictionaryRef createItemQuery(const QString& service, const QString& account)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    if (query == nullptr) {
        return nullptr;
    }

    CFRef<CFStringRef> cfService(createCFString(service));
    CFRef<CFStringRef> cfAccount(createCFString(account));
    if (cfService.get() == nullptr || cfAccount.get() == nullptr) {
        CFRelease(query);
        return nullptr;
    }

    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cfService.get());
    CFDictionarySetValue(query, kSecAttrAccount, cfAccount.get());
    CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
    return query;
}

}

Keychain::Status Keychain::read(const QString& service, const QString& account, QByteArray& data, QString* error)
{
    if (!data.isEmpty()) {
        data.fill('\0');
    }
    data.clear();

    CFRef<CFMutableDictionaryRef> query(createItemQuery(service, account));
    if (query.get() == nullptr) {
        return fail("Building the Keychain query", errSecAllocate, error);
    }
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);

    CFRef<CFTypeRef> result;
    OSStatus status = SecItemCopyMatching(query.get(), result.receive());
    if (status == errSecItemNotFound) {
        return Status::NotFound;
    }
    else if (status != errSecSuccess) {
        return fail("SecItemCopyMatching", status, error);
    }
    else if (result.get() == nullptr || CFGetTypeID(result.get()) != CFDataGetTypeID()) {
        return fail("SecItemCopyMatching (unexpected result type)", errSecInternalError, error);
    }

    CFDataRef cfData = static_cast<CFDataRef>(result.get());
    data = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(cfData)), (int)CFDataGetLength(cfData));
    return Status::Ok;
}

Keychain::Status Keychain::write(const QString& service, const QString& account, const QByteArray& data, QString* error)
{
    CFRef<CFMutableDictionaryRef> query(createItemQuery(service, account));
    CFRef<CFDataRef> cfData(CFDataCreate(kCFAllocatorDefault,
                                         reinterpret_cast<const UInt8*>(data.constData()),
                                         data.size()));
    CFRef<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                       &kCFTypeDictionaryKeyCallBacks,
                                                                       &kCFTypeDictionaryValueCallBacks));
    if (query.get() == nullptr || cfData.get() == nullptr || attributes.get() == nullptr) {
        return fail("Building the Keychain query", errSecAllocate, error);
    }
    CFDictionarySetValue(attributes.get(), kSecValueData, cfData.get());
    CFDictionarySetValue(attributes.get(), kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    // Replace the data of an existing item in place, so there is never a window
    // in which no key is stored.
    OSStatus status = SecItemUpdate(query.get(), attributes.get());
    if (status == errSecSuccess) {
        return Status::Ok;
    }
    else if (status != errSecItemNotFound) {
        return fail("SecItemUpdate", status, error);
    }

    CFRef<CFStringRef> label(createCFString(QStringLiteral("Moonlight client identity key")));
    CFDictionarySetValue(query.get(), kSecValueData, cfData.get());
    CFDictionarySetValue(query.get(), kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    if (label.get() != nullptr) {
        CFDictionarySetValue(query.get(), kSecAttrLabel, label.get());
    }

    status = SecItemAdd(query.get(), nullptr);
    if (status != errSecSuccess) {
        return fail("SecItemAdd", status, error);
    }

    return Status::Ok;
}

Keychain::Status Keychain::remove(const QString& service, const QString& account, QString* error)
{
    CFRef<CFMutableDictionaryRef> query(createItemQuery(service, account));
    if (query.get() == nullptr) {
        return fail("Building the Keychain query", errSecAllocate, error);
    }

    OSStatus status = SecItemDelete(query.get());
    if (status == errSecItemNotFound) {
        return Status::NotFound;
    }
    else if (status != errSecSuccess) {
        return fail("SecItemDelete", status, error);
    }

    return Status::Ok;
}
