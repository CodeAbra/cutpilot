#include "cutpilot/secrets/SecretStore.h"

#include "cutpilot/secrets/KeychainStore.h"

namespace cutpilot::secrets {

bool KeychainSecretStore::available() const
{
    return KeychainStore::available();
}

bool KeychainSecretStore::writeSecret(const QString &service,
                                      const QString &account,
                                      const QString &value)
{
    return KeychainStore::writeSecret(service, account, value);
}

QString KeychainSecretStore::readSecret(const QString &service,
                                        const QString &account) const
{
    return KeychainStore::readSecret(service, account);
}

bool KeychainSecretStore::removeSecret(const QString &service,
                                       const QString &account)
{
    return KeychainStore::removeSecret(service, account);
}

bool KeychainSecretStore::hasSecret(const QString &service,
                                    const QString &account) const
{
    return KeychainStore::hasSecret(service, account);
}

} // namespace cutpilot::secrets
