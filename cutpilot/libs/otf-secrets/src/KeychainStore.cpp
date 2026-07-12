#include "cutpilot/secrets/KeychainStore.h"

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace cutpilot::secrets {

#ifdef Q_OS_MACOS

namespace {

CFStringRef cfString(const QString &value)
{
    return CFStringCreateWithCharacters(
        kCFAllocatorDefault, reinterpret_cast<const UniChar *>(value.utf16()),
        value.length());
}

// A mutable query addressing one generic-password item.
CFMutableDictionaryRef itemQuery(const QString &service, const QString &account)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFStringRef serviceRef = cfString(service);
    CFStringRef accountRef = cfString(account);
    CFDictionarySetValue(query, kSecAttrService, serviceRef);
    CFDictionarySetValue(query, kSecAttrAccount, accountRef);
    CFRelease(serviceRef);
    CFRelease(accountRef);
    return query;
}

} // namespace

bool KeychainStore::available()
{
    return true;
}

bool KeychainStore::writeSecret(const QString &service, const QString &account,
                                const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()),
        bytes.size());

    // Replace an existing item in place; add it when absent.
    CFMutableDictionaryRef query = itemQuery(service, account);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, update);
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, data);
        status = SecItemAdd(query, nullptr);
    }

    CFRelease(update);
    CFRelease(query);
    CFRelease(data);
    return status == errSecSuccess;
}

QString KeychainStore::readSecret(const QString &service, const QString &account)
{
    CFMutableDictionaryRef query = itemQuery(service, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || result == nullptr)
        return QString();

    const auto data = static_cast<CFDataRef>(result);
    const QString value = QString::fromUtf8(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        int(CFDataGetLength(data)));
    CFRelease(result);
    return value;
}

#else

bool KeychainStore::available()
{
    return false;
}

bool KeychainStore::writeSecret(const QString &, const QString &, const QString &)
{
    return false;
}

QString KeychainStore::readSecret(const QString &, const QString &)
{
    return QString();
}

#endif

} // namespace cutpilot::secrets
