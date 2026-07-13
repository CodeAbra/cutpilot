#include "WorkflowStore.h"

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/WorkflowJson.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace cutpilot::app {

namespace {

constexpr int kSaveDebounceMs = 750;
const QLatin1String kFileName("workflow.json");

} // namespace

WorkflowStore::WorkflowStore(core::NodeGraph *graph, QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_directory(defaultDirectory())
    , m_name(QStringLiteral("Untitled Workflow"))
{
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDebounceMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { saveNow(); });
}

QString WorkflowStore::defaultDirectory()
{
    const QString override = qEnvironmentVariable("CUTPILOT_WORKSPACE_DIR");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/workspace");
}

void WorkflowStore::setDirectory(const QString &directory)
{
    m_directory = directory;
}

QString WorkflowStore::filePath() const
{
    return m_directory + QLatin1Char('/') + kFileName;
}

void WorkflowStore::setName(const QString &name)
{
    const QString trimmed = name.trimmed();
    const QString applied =
        trimmed.isEmpty() ? QStringLiteral("Untitled Workflow") : trimmed;
    if (m_name == applied)
        return;
    m_name = applied;
    emit nameChanged();
    scheduleSave();
}

void WorkflowStore::scheduleSave()
{
    setState(State::Pending);
    m_saveTimer->start();
}

bool WorkflowStore::saveNow()
{
    m_saveTimer->stop();

    if (!QDir().mkpath(m_directory)) {
        setState(State::Failed,
                 QStringLiteral("could not create %1").arg(m_directory));
        return false;
    }

    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        setState(State::Failed, file.errorString());
        return false;
    }
    const QJsonDocument document(core::workflowToJson(*m_graph, m_name));
    file.write(document.toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        setState(State::Failed, file.errorString());
        return false;
    }

    m_savedAt = QDateTime::currentDateTime();
    setState(State::Saved);
    return true;
}

bool WorkflowStore::load()
{
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;

    QString name;
    if (!core::workflowFromJson(document.object(), *m_graph, &name))
        return false;

    if (!name.isEmpty() && name != m_name) {
        m_name = name;
        emit nameChanged();
    }
    m_savedAt = QFileInfo(file).lastModified();
    setState(State::Saved);
    return true;
}

void WorkflowStore::setState(State state, const QString &reason)
{
    m_failureReason = reason;
    if (m_state == state && reason.isEmpty()) {
        if (state != State::Saved)
            return;
        // A fresh save refreshes the timestamp even when the state name holds.
    }
    m_state = state;
    emit stateChanged();
}

} // namespace cutpilot::app
