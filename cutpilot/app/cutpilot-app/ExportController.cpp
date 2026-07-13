#include "ExportController.h"

#include "cutpilot/core/CompositePlan.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/timeline/ProjectStore.h"
#include "cutpilot/ipc/ConvertClient.h"
#include "cutpilot/render/CompositorEngine.h"
#include "cutpilot/render/CompositorService.h"
#include "cutpilot/render/NodeLayerItem.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QStandardPaths>

#include <memory>

using namespace cutpilot;
using namespace cutpilot::core::timeline;

namespace {

constexpr int kSequenceRate = 24;

QString reportText(const core::ComfyImportOutcome &outcome)
{
    QStringList lines;
    lines << QStringLiteral("Imported %1 node(s), %2 wire(s).")
                 .arg(outcome.nodeIds.size())
                 .arg(outcome.connectionCount);
    if (outcome.droppedEdges > 0)
        lines << QStringLiteral("%1 link(s) had no free compatible port and "
                                "were dropped.")
                     .arg(outcome.droppedEdges);
    lines << QString();
    for (const core::ComfyImportRow &row : outcome.report) {
        lines << QStringLiteral("#%1  %2  ->  %3   [%4]")
                     .arg(row.comfyId)
                     .arg(row.comfyType, row.mappedKind, row.tier);
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

ExportController::ExportController(render::NodeLayerItem *layer,
                                   ipc::ConvertClient *convert,
                                   render::CompositorService *media,
                                   QObject *parent)
    : QObject(parent)
    , m_layer(layer)
    , m_convert(convert)
    , m_media(media)
{
    connect(m_convert, &ipc::ConvertClient::timelineExported, this,
            &ExportController::onTimelineExported);
    connect(m_convert, &ipc::ConvertClient::timelineExportFailed, this,
            &ExportController::onTimelineExportFailed);
}

QString ExportController::projectPath() const
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/project.json");
}

QHash<int, qint64> ExportController::videoDurations() const
{
    QHash<int, qint64> durations;
    if (!m_media)
        return durations;
    for (const core::Node &node : m_layer->graph().nodes()) {
        if (node.kind != core::NodeKind::Video)
            continue;
        const qint64 ms = m_media->videoDurationMs(node.id);
        if (ms > 0)
            durations.insert(node.id, qMax<qint64>(1, ms * kSequenceRate / 1000));
    }
    return durations;
}

QHash<int, QString> ExportController::renderCompositeTerminals(
    const QString &directory)
{
    QHash<int, QString> rendered;
    const QVector<int> terminals = compositeTerminals(m_layer->graph());
    if (terminals.isEmpty())
        return rendered;

    render::CompositorEngine engine;
    if (!engine.adoptHeadlessDevice())
        return rendered; // reported by the timeline builder as skipped

    for (int nodeId : terminals) {
        const core::CompositePlan plan =
            core::buildCompositePlan(m_layer->graph(), nodeId);
        if (!plan.valid)
            continue;
        for (int sourceId : plan.sourceNodeIds) {
            const QImage source = m_layer->nodeMediaImage(sourceId);
            if (!source.isNull())
                engine.setSource(sourceId, source,
                                 m_layer->nodeMediaVersion(sourceId));
        }
        const QImage image = engine.evaluateToImage(plan);
        if (image.isNull())
            continue;
        const QString path = QDir(directory).filePath(
            QStringLiteral("composite-%1.png").arg(nodeId));
        if (image.save(path, "PNG"))
            rendered.insert(nodeId, path);
    }
    return rendered;
}

void ExportController::exportTimelineTo(const QString &directory,
                                        const QString &baseName)
{
    if (m_pendingWriters > 0) {
        emit statusChanged(QStringLiteral("An export is already running"));
        return;
    }
    if (!QDir().mkpath(directory)) {
        emit statusChanged(
            QStringLiteral("Could not create %1").arg(directory));
        emit exportFinished(false, directory);
        return;
    }

    GraphTimelineOptions options;
    options.projectName = baseName;
    options.fps = Fps{ kSequenceRate, 1 };
    options.durationOverrides = videoDurations();
    options.renderedMedia = renderCompositeTerminals(directory);

    m_bundle = writeExportBundle(m_layer->graph(), options, directory, baseName);
    if (!m_bundle.ok) {
        emit statusChanged(QStringLiteral("Export failed: %1").arg(m_bundle.error));
        emit exportFinished(false, directory);
        return;
    }

    m_exportFailed = false;
    m_pendingWriters = 2;
    emit statusChanged(QStringLiteral("Exporting to %1…").arg(directory));
    m_convert->exportTimeline(QStringLiteral("fcpxml"),
                              exportPayload(m_bundle, QStringLiteral("fcpxml")));
    m_convert->exportTimeline(QStringLiteral("otio"),
                              exportPayload(m_bundle, QStringLiteral("otio")));
}

void ExportController::onTimelineExported(const QString &format,
                                          const QString &path)
{
    Q_UNUSED(format);
    Q_UNUSED(path);
    if (m_pendingWriters > 0) {
        --m_pendingWriters;
        settleExportIfDone();
    }
}

void ExportController::onTimelineExportFailed(const QString &format,
                                              const QString &error)
{
    if (m_pendingWriters > 0) {
        --m_pendingWriters;
        m_exportFailed = true;
        emit statusChanged(
            QStringLiteral("%1 export failed: %2").arg(format, error));
        settleExportIfDone();
    }
}

void ExportController::settleExportIfDone()
{
    if (m_pendingWriters > 0)
        return;
    if (m_exportFailed) {
        emit exportFinished(false, m_bundle.directory);
        return;
    }
    QString message =
        QStringLiteral("Exported %1 shot(s): %2, %3, %4")
            .arg(m_bundle.shotNodeIds.size())
            .arg(QFileInfo(m_bundle.edlPath).fileName(),
                 QFileInfo(m_bundle.fcpxmlPath).fileName(),
                 QFileInfo(m_bundle.otioPath).fileName());
    if (!m_bundle.skipped.isEmpty())
        message += QStringLiteral("  (skipped: %1)")
                       .arg(m_bundle.skipped.join(QStringLiteral("; ")));
    emit statusChanged(message);
    emit exportFinished(true, m_bundle.directory);
}

void ExportController::exportTimelineInteractive(QWidget *parent)
{
    const QString directory = QFileDialog::getExistingDirectory(
        parent, QStringLiteral("Choose an export folder"));
    if (directory.isEmpty())
        return;
    exportTimelineTo(directory, QStringLiteral("cut"));
}

QString ExportController::addResultsToTimeline()
{
    GraphTimelineOptions options;
    options.fps = Fps{ kSequenceRate, 1 };
    options.durationOverrides = videoDurations();
    const GraphTimelineResult result =
        timelineFromGraph(m_layer->graph(), options);
    if (result.shotNodeIds.isEmpty()) {
        emit statusChanged(
            QStringLiteral("The canvas holds no results to add"));
        return QString();
    }

    const QString path = projectPath();
    TimelineProject project;
    QString error;
    if (QFileInfo::exists(path) && !loadProject(path, &project, &error)) {
        emit statusChanged(
            QStringLiteral("Could not read the project: %1").arg(error));
        return QString();
    }

    const Sequence &sequence = result.project.sequences.first();
    int added = 0;
    for (const Track &track : sequence.tracks) {
        for (const Segment &segment : track.segments) {
            if (segment.type == SegmentType::Gap)
                continue;
            const MediaAsset *asset = result.project.assetById(segment.assetId);
            if (!asset)
                continue;
            appendGeneratorSegment(project, *asset);
            ++added;
        }
    }

    if (!saveProject(project, path, &error)) {
        emit statusChanged(
            QStringLiteral("Could not save the project: %1").arg(error));
        return QString();
    }
    emit statusChanged(QStringLiteral("Added %1 result(s) to the timeline (%2)")
                           .arg(added)
                           .arg(path));
    return path;
}

void ExportController::importComfyInteractive(QWidget *parent)
{
    const QString path = QFileDialog::getOpenFileName(
        parent, QStringLiteral("Choose a ComfyUI workflow"), QString(),
        QStringLiteral("ComfyUI workflows (*.json)"));
    if (path.isEmpty())
        return;
    importComfyFromFile(path, parent);
}

void ExportController::importComfyFromFile(const QString &path,
                                           QWidget *dialogParent)
{
    if (m_comfyBusy) {
        emit statusChanged(
            QStringLiteral("A workflow import is already running"));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit statusChanged(
            QStringLiteral("Could not read %1").arg(path));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit statusChanged(
            QStringLiteral("Not a workflow document: %1").arg(path));
        return;
    }

    m_comfyBusy = true;
    QPointer<QWidget> guard(dialogParent);
    auto imported = std::make_shared<QMetaObject::Connection>();
    auto failed = std::make_shared<QMetaObject::Connection>();
    auto settle = [this, imported, failed] {
        QObject::disconnect(*imported);
        QObject::disconnect(*failed);
        m_comfyBusy = false;
    };
    *imported = connect(
        m_convert, &ipc::ConvertClient::comfyImported, this,
        [this, guard, settle](const QJsonObject &result) {
            settle();
            const core::ComfyImportOutcome outcome =
                m_layer->importComfyWorkflow(result, QPointF(0, 0));
            if (!outcome.ok) {
                emit statusChanged(QStringLiteral("Import failed: %1")
                                       .arg(outcome.error));
                return;
            }
            emit statusChanged(
                QStringLiteral("Imported %1 node(s) from the workflow")
                    .arg(outcome.nodeIds.size()));
            emit comfyImportFinished(outcome);
            if (guard)
                QMessageBox::information(guard,
                                         QStringLiteral("Workflow imported"),
                                         reportText(outcome));
        });
    *failed = connect(m_convert, &ipc::ConvertClient::comfyImportFailed, this,
                      [this, settle](const QString &error) {
                          settle();
                          emit statusChanged(
                              QStringLiteral("Import failed: %1").arg(error));
                      });
    m_convert->importComfyWorkflow(document.object());
}

void ExportController::sendToResolve()
{
    if (m_resolveBusy) {
        emit statusChanged(
            QStringLiteral("A Resolve hand-off is already running"));
        return;
    }
    if (!m_bundle.ok || !QFileInfo::exists(m_bundle.fcpxmlPath)) {
        emit statusChanged(
            QStringLiteral("Export the timeline first, then send it to "
                           "Resolve"));
        return;
    }
    m_resolveBusy = true;
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(
        m_convert, &ipc::ConvertClient::resolveImportFinished, this,
        [this, connection](bool ok, const QString &reason,
                           const QString &detail) {
            QObject::disconnect(*connection);
            m_resolveBusy = false;
            if (ok) {
                emit statusChanged(
                    QStringLiteral("Timeline imported into Resolve"));
                return;
            }
            emit statusChanged(detail.isEmpty()
                                   ? QStringLiteral("Resolve import failed: %1")
                                         .arg(reason)
                                   : detail);
        });
    m_convert->importIntoResolve(m_bundle.fcpxmlPath);
}
