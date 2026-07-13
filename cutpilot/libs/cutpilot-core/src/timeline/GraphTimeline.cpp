#include "cutpilot/core/timeline/GraphTimeline.h"

#include <QFileInfo>
#include <QImageReader>
#include <QSet>

#include <algorithm>

namespace cutpilot::core::timeline {

namespace {

bool producesMediaFromInputs(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Generate:
    case NodeKind::Blend:
    case NodeKind::Mask:
    case NodeKind::Key:
    case NodeKind::Transform:
        return true;
    default:
        return false;
    }
}

bool isCompositeKind(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Blend:
    case NodeKind::Mask:
    case NodeKind::Key:
    case NodeKind::Transform:
        return true;
    default:
        return false;
    }
}

struct ShotCandidate {
    int nodeId = 0;
    QPointF worldPos;
    QString mediaPath;
    bool produced = false;
    int width = 0;
    int height = 0;
};

} // namespace

GraphTimelineResult timelineFromGraph(const NodeGraph &graph,
                                      const GraphTimelineOptions &options)
{
    GraphTimelineResult result;
    result.project.name = options.projectName;

    Sequence sequence;
    sequence.name = options.sequenceName;
    sequence.fps = options.fps;
    sequence.width = options.width;
    sequence.height = options.height;

    const qint64 stillFrames = options.stillDurationFrames > 0
        ? options.stillDurationFrames
        : qint64(4) * qMax(1, options.fps.nominalRate());

    // An ingredient feeds a downstream producer; its pixels live on inside
    // that producer's output, so it is not a shot of its own.
    QSet<int> ingredients;
    for (const Connection &connection : graph.connections()) {
        const Node *consumer = graph.nodeById(connection.toNodeId);
        if (consumer && producesMediaFromInputs(consumer->kind))
            ingredients.insert(connection.fromNodeId);
    }

    QVector<ShotCandidate> candidates;
    for (const Node &node : graph.nodes()) {
        if (ingredients.contains(node.id))
            continue;

        ShotCandidate candidate;
        candidate.nodeId = node.id;
        candidate.worldPos = node.worldPos;

        if (options.renderedMedia.contains(node.id)) {
            candidate.mediaPath = options.renderedMedia.value(node.id);
            candidate.produced = true;
        } else if (node.kind == NodeKind::Generate && !node.resultPath.isEmpty()) {
            candidate.mediaPath = node.resultPath;
            candidate.produced = true;
            candidate.width = node.resultWidth;
            candidate.height = node.resultHeight;
        } else if ((node.kind == NodeKind::Still || node.kind == NodeKind::Video)
                   && !node.mediaPath.isEmpty()) {
            candidate.mediaPath = node.mediaPath;
        } else {
            if (isCompositeKind(node.kind))
                result.skipped << QStringLiteral(
                                      "%1: composited output was not rendered "
                                      "to a file")
                                      .arg(node.title.isEmpty()
                                               ? QStringLiteral("node %1")
                                                     .arg(node.id)
                                               : node.title);
            continue;
        }
        candidates.append(candidate);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const ShotCandidate &a, const ShotCandidate &b) {
                  if (a.worldPos.x() != b.worldPos.x())
                      return a.worldPos.x() < b.worldPos.x();
                  if (a.worldPos.y() != b.worldPos.y())
                      return a.worldPos.y() < b.worldPos.y();
                  return a.nodeId < b.nodeId;
              });

    Track track;
    track.name = QStringLiteral("V1");
    track.type = TrackType::Video;

    qint64 cursor = 0;
    for (const ShotCandidate &candidate : candidates) {
        MediaAsset asset;
        asset.id = QStringLiteral("node-%1").arg(candidate.nodeId);
        asset.filePath = candidate.mediaPath;
        asset.name = QFileInfo(candidate.mediaPath).fileName();
        asset.fps = options.fps;
        asset.width = candidate.width;
        asset.height = candidate.height;
        if (asset.width == 0 || asset.height == 0) {
            const QSize probed = QImageReader(candidate.mediaPath).size();
            if (probed.isValid()) {
                asset.width = probed.width();
                asset.height = probed.height();
            }
        }

        const qint64 duration = options.durationOverrides.contains(
                                    candidate.nodeId)
            ? qMax<qint64>(1, options.durationOverrides.value(candidate.nodeId))
            : stillFrames;
        asset.durationFrames = duration;

        Segment segment;
        segment.id = QStringLiteral("shot-%1").arg(candidate.nodeId);
        segment.type =
            candidate.produced ? SegmentType::Generator : SegmentType::Clip;
        segment.assetId = asset.id;
        segment.timelineIn = cursor;
        segment.timelineOut = cursor + duration;
        segment.sourceIn = 0;
        segment.sourceOut = duration;
        cursor += duration;

        result.project.assets.append(asset);
        track.segments.append(segment);
        result.shotNodeIds.append(candidate.nodeId);
    }

    sequence.tracks.append(track);
    result.project.sequences.append(sequence);
    return result;
}

} // namespace cutpilot::core::timeline
