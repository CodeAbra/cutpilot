#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <optional>

namespace cutpilot::ipc {

// The per-node result cache for pipeline runs. An entry is keyed by the
// signature of everything that shaped the result — the model, the resolved
// prompt, the requested size and seed, and the digests of the actual input
// bytes consumed — so an identical recompute is a hit and any effective
// change is a miss. A hit is only served while its result file still exists.
class ResultCache {
public:
    struct Entry {
        QString resultPath;
        QString resultDigest;
        double costUsd = 0.0;
        int width = 0;
        int height = 0;
        // Remembers whether the cached result was a video, so a reused result
        // re-enters the same decode path it first took.
        QString kind;
    };

    // The canonical signature. Every field is length-prefixed before hashing,
    // so no combination of values can collide with another.
    static QString signature(const QString &modelId, const QString &prompt,
                             int width, int height, int seed,
                             const QVector<QString> &inputDigests);

    void store(const QString &signature, const Entry &entry);
    std::optional<Entry> lookup(const QString &signature) const;

private:
    QHash<QString, Entry> m_entries;
};

} // namespace cutpilot::ipc
