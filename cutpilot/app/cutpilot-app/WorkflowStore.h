#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

namespace cutpilot::core {
class NodeGraph;
}

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace cutpilot::app {

// The workflow's home on disk: the graph autosaves as a JSON document a
// moment after every change, loads back on launch, and reports its live
// save state for the top bar's indicator. Writes are atomic, so a crash
// mid-save never corrupts the last good document.
class WorkflowStore : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Pending,
        Saved,
        Failed
    };
    Q_ENUM(State)

    explicit WorkflowStore(core::NodeGraph *graph, QObject *parent = nullptr);

    // The workspace directory: the CUTPILOT_WORKSPACE_DIR environment override,
    // or the per-user application data location.
    static QString defaultDirectory();

    QString directory() const { return m_directory; }
    void setDirectory(const QString &directory);

    QString filePath() const;

    QString name() const { return m_name; }
    void setName(const QString &name);

    State state() const { return m_state; }
    QDateTime savedAt() const { return m_savedAt; }
    QString failureReason() const { return m_failureReason; }

    // Debounced autosave: burst edits coalesce into one write.
    void scheduleSave();
    bool saveNow();

    // Write a pending debounced save immediately — the quit path, so an edit
    // made moments before closing never silently vanishes.
    void flushPendingSave();

    // Populate the (empty) graph from the stored document. False when there is
    // no document or it fails validation.
    bool load();

signals:
    void nameChanged();
    void stateChanged();

private:
    void setState(State state, const QString &reason = QString());

    core::NodeGraph *m_graph = nullptr;
    QString m_directory;
    QString m_name;
    QTimer *m_saveTimer = nullptr;
    State m_state = State::Idle;
    QDateTime m_savedAt;
    QString m_failureReason;
};

} // namespace cutpilot::app
