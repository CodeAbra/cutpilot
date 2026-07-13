#pragma once

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/timeline/Timeline.h"

#include <QHash>

namespace cutpilot::core::timeline {

struct GraphTimelineOptions {
    QString projectName = QStringLiteral("CutPilot");
    QString sequenceName = QStringLiteral("Canvas");
    Fps fps;
    int width = 1920;
    int height = 1080;

    // Frames a still occupies on the timeline; zero means four seconds at
    // the sequence rate.
    qint64 stillDurationFrames = 0;

    // Per-node duration in frames, for media whose length the graph knows
    // (videos probed by the playback stack). Wins over stillDurationFrames.
    QHash<int, qint64> durationOverrides;

    // Per-node media file standing in for a node that holds no file of its
    // own — a compositing node rendered to disk for the export. The node
    // exports as produced media.
    QHash<int, QString> renderedMedia;
};

struct GraphTimelineResult {
    // One sequence, one video track, shots laid contiguously from frame 0.
    TimelineProject project;

    // The exported nodes in timeline order.
    QVector<int> shotNodeIds;

    // Media-backed nodes that could not be exported, with the reason.
    QStringList skipped;
};

// The edit the canvas implies. A node is a shot when it carries media — a
// generation result, a Still/Video file, or a supplied render — and none of
// its outgoing wires feeds a node that produces further media from it (such
// a node is an ingredient of the downstream result, not a shot of its own).
// Shots read in canvas order: left to right, then top to bottom, ties by id.
// Generation results and supplied renders become Generator segments; file
// media becomes Clip segments.
GraphTimelineResult timelineFromGraph(const NodeGraph &graph,
                                      const GraphTimelineOptions &options);

// The compositing nodes that are shots — they feed no downstream media
// producer — but hold no file of their own. An export renders each to disk
// and passes the files through renderedMedia so they land on the timeline.
QVector<int> compositeTerminals(const NodeGraph &graph);

} // namespace cutpilot::core::timeline
