#pragma once

#include <QString>

namespace cutpilot::secrets {

// Where BYOK secrets live, behind an injectable seam. The production store
// is the operating system's keychain; a surface that takes the store as a
// dependency stays testable against a stand-in that never touches the
// user's real keychain entries.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    virtual bool available() const = 0;

    // Create or replace the secret.
    virtual bool writeSecret(const QString &service, const QString &account,
                             const QString &value) = 0;

    // The stored secret, or an empty string when absent or unavailable.
    virtual QString readSecret(const QString &service,
                               const QString &account) const = 0;

    // Delete the secret. True when it is absent afterwards.
    virtual bool removeSecret(const QString &service,
                              const QString &account) = 0;

    // Presence only — the secret's bytes are never read for this check.
    virtual bool hasSecret(const QString &service,
                           const QString &account) const = 0;
};

// The system keychain as a SecretStore.
class KeychainSecretStore final : public SecretStore {
public:
    bool available() const override;
    bool writeSecret(const QString &service, const QString &account,
                     const QString &value) override;
    QString readSecret(const QString &service,
                       const QString &account) const override;
    bool removeSecret(const QString &service, const QString &account) override;
    bool hasSecret(const QString &service,
                   const QString &account) const override;
};

} // namespace cutpilot::secrets
