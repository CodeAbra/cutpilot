#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include "cutpilot/core/ComfyImport.h"
#include "cutpilot/core/timeline/ExportBundle.h"

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace cutpilot::ipc {
class ConvertClient;
}
namespace cutpilot::render {
class CompositorService;
class NodeLayerItem;
}

// The canvas's way out: exports the board as an EDL + FCPXML + OTIO bundle
// with its media, lands results in the project model as Generator segments,
// imports ComfyUI workflows through the convert service, and hands exports
// to DaVinci Resolve. Interactive entry points raise dialogs; the export
// itself is a plain method so evidence runs can drive it headlessly.
class ExportController : public QObject {
    Q_OBJECT

public:
    ExportController(cutpilot::render::NodeLayerItem *layer,
                     cutpilot::ipc::ConvertClient *convert,
                     cutpilot::render::CompositorService *media,
                     QObject *parent = nullptr);

    // Export the board into <directory>/<baseName>.{edl,fcpxml,otio} plus
    // media/. Terminal compositing nodes are rendered to files first, video
    // durations come from the playback stack, and the interchange files are
    // written by the convert service. Completion arrives via exportFinished.
    void exportTimelineTo(const QString &directory, const QString &baseName);

    // Interactive wrappers over the same flows.
    void exportTimelineInteractive(QWidget *parent);
    void importComfyInteractive(QWidget *parent);

    // Import a ComfyUI workflow file onto the board. One import runs at a
    // time; a call while one is outstanding is refused through the status.
    // With a dialogParent the tier report is raised as a dialog.
    void importComfyFromFile(const QString &path, QWidget *dialogParent);

    // Append the board's shots to the project model as segments (produced
    // media lands as Generator) and persist it. Returns the project path,
    // empty on failure with the reason in status.
    QString addResultsToTimeline();

    // Hand the last export's FCPXML to a running DaVinci Resolve.
    void sendToResolve();

    QString projectPath() const;
    const cutpilot::core::timeline::ExportBundle &lastBundle() const
    {
        return m_bundle;
    }

signals:
    // Human-readable progress for the status strip.
    void statusChanged(const QString &message);

    // The bundle settled: EDL written and both interchange writers
    // answered. ok is false when any piece failed.
    void exportFinished(bool ok, const QString &directory);

    // A ComfyUI import landed; the report lists one row per foreign node.
    void comfyImportFinished(const cutpilot::core::ComfyImportOutcome &outcome);

private:
    void onTimelineExported(const QString &format, const QString &path);
    void onTimelineExportFailed(const QString &format, const QString &error);
    void settleExportIfDone();

    // Render every terminal compositing node to a PNG under directory;
    // returns nodeId → file for the timeline options.
    QHash<int, QString> renderCompositeTerminals(const QString &directory);
    QHash<int, qint64> videoDurations() const;

    cutpilot::render::NodeLayerItem *m_layer = nullptr;
    cutpilot::ipc::ConvertClient *m_convert = nullptr;
    cutpilot::render::CompositorService *m_media = nullptr;

    cutpilot::core::timeline::ExportBundle m_bundle;
    int m_pendingWriters = 0;
    bool m_exportFailed = false;

    // The convert client is shared; an unguarded second request would fan
    // its answer into every listener. One outstanding request per flow.
    bool m_comfyBusy = false;
    bool m_resolveBusy = false;
};
