#pragma once

#include "cutpilot/core/timeline/GraphTimeline.h"

#include <QJsonObject>

namespace cutpilot::core::timeline {

// A self-contained export on disk: the media the timeline references copied
// into <directory>/media/, an EDL written beside it, and the interchange
// output paths reserved for the FCPXML/OTIO writers. The bundled project's
// asset paths point at the copies, so the folder travels whole.
struct ExportBundle {
    bool ok = false;
    QString error;

    QString directory;
    QString edlPath;
    QString fcpxmlPath;
    QString otioPath;

    TimelineProject project;
    QVector<int> shotNodeIds;
    QStringList skipped;
};

// Assemble the bundle from the canvas. Fails whole — with the reason —
// when the graph implies no shots or a referenced media file cannot be
// copied; a partial bundle is never left behind as a success.
ExportBundle writeExportBundle(const NodeGraph &graph,
                               const GraphTimelineOptions &options,
                               const QString &directory,
                               const QString &baseName);

// The convert service's timeline payload for this bundle — title, exact
// rate, dimensions, the shots with their bundled media paths, and the
// output path for the requested format ("fcpxml" or "otio").
QJsonObject exportPayload(const ExportBundle &bundle, const QString &format);

} // namespace cutpilot::core::timeline
