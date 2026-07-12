#include "cutpilot/ipc/ResultCache.h"

#include <QCryptographicHash>
#include <QFileInfo>

namespace cutpilot::ipc {

QString ResultCache::signature(const QString &modelId, const QString &prompt,
                               int width, int height, int seed,
                               const QVector<QString> &inputDigests)
{
    QByteArray canonical;
    const auto append = [&canonical](const QByteArray &field) {
        canonical += QByteArray::number(field.size());
        canonical += ':';
        canonical += field;
    };
    append(modelId.toUtf8());
    append(prompt.toUtf8());
    append(QByteArray::number(width));
    append(QByteArray::number(height));
    append(QByteArray::number(seed));
    for (const QString &digest : inputDigests)
        append(digest.toUtf8());

    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

void ResultCache::store(const QString &signature, const Entry &entry)
{
    m_entries.insert(signature, entry);
}

std::optional<ResultCache::Entry> ResultCache::lookup(const QString &signature) const
{
    const auto it = m_entries.constFind(signature);
    if (it == m_entries.constEnd())
        return std::nullopt;
    if (!QFileInfo::exists(it->resultPath))
        return std::nullopt;
    return *it;
}

} // namespace cutpilot::ipc
