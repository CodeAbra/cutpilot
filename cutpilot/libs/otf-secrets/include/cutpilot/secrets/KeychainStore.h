#pragma once

#include <QString>

namespace cutpilot::secrets {

// BYOK secret storage in the operating system's keychain. Secrets are the
// user's own vendor API keys: they are written once from the add-a-key flow
// and read back by the generation service, never logged and never persisted
// anywhere else. Items are generic passwords addressed by service + account,
// so every process on the machine that speaks the platform keychain sees the
// same entry.
class KeychainStore {
public:
    // True on platforms where a system keychain is wired up.
    static bool available();

    // Create or replace the secret. Returns false when the platform has no
    // keychain or the write was refused.
    static bool writeSecret(const QString &service, const QString &account,
                            const QString &value);

    // The stored secret, or an empty string when absent or unavailable.
    static QString readSecret(const QString &service, const QString &account);
};

} // namespace cutpilot::secrets
