#include "cutpilot/core/timeline/ExportBundle.h"

#include "cutpilot/core/timeline/EdlWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

namespace cutpilot::core::timeline {

namespace {

ExportBundle failed(const QString &error)
{
    ExportBundle bundle;
    bundle.error = error;
    return bundle;
}

// A destination file name for the asset that no earlier copy claimed.
QString claimName(const QString &preferred, QSet<QString> &taken)
{
    const QFileInfo info(preferred);
    const QString stem = info.completeBaseName();
    const QString suffix =
        info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    QString candidate = preferred;
    for (int attempt = 2; taken.contains(candidate.toLower()); ++attempt)
        candidate = QStringLiteral("%1-%2%3").arg(stem).arg(attempt).arg(suffix);
    taken.insert(candidate.toLower());
    return candidate;
}

} // namespace

ExportBundle writeExportBundle(const NodeGraph &graph,
                               const GraphTimelineOptions &options,
                               const QString &directory,
                               const QString &baseName)
{
    if (directory.isEmpty() || baseName.trimmed().isEmpty())
        return failed(QStringLiteral("export needs a folder and a name"));

    GraphTimelineResult timeline = timelineFromGraph(graph, options);
    if (timeline.shotNodeIds.isEmpty()) {
        ExportBundle bundle =
            failed(QStringLiteral("the canvas holds no exportable results"));
        bundle.skipped = timeline.skipped;
        return bundle;
    }

    QDir dir(directory);
    const QString mediaDirName = QStringLiteral("media");
    if (!dir.mkpath(mediaDirName))
        return failed(QStringLiteral("could not create %1")
                          .arg(dir.filePath(mediaDirName)));

    QSet<QString> taken;
    for (MediaAsset &asset : timeline.project.assets) {
        const QFileInfo source(asset.filePath);
        if (!source.isFile())
            return failed(QStringLiteral("media file is missing: %1")
                              .arg(asset.filePath));
        const QString fileName = claimName(source.fileName(), taken);
        const QString destination =
            dir.filePath(mediaDirName + QLatin1Char('/') + fileName);
        if (QFile::exists(destination))
            QFile::remove(destination);
        if (!QFile::copy(asset.filePath, destination))
            return failed(QStringLiteral("could not copy %1 into the bundle")
                              .arg(asset.filePath));
        asset.filePath = QFileInfo(destination).absoluteFilePath();
        asset.name = fileName;
    }

    ExportBundle bundle;
    bundle.project = timeline.project;
    bundle.shotNodeIds = timeline.shotNodeIds;
    bundle.skipped = timeline.skipped;
    bundle.directory = QDir(directory).absolutePath();
    bundle.edlPath = dir.absoluteFilePath(baseName + QStringLiteral(".edl"));
    bundle.fcpxmlPath =
        dir.absoluteFilePath(baseName + QStringLiteral(".fcpxml"));
    bundle.otioPath = dir.absoluteFilePath(baseName + QStringLiteral(".otio"));

    const QString edl = writeEdl(bundle.project.sequences.first(),
                                 bundle.project, options.projectName);
    QFile edlFile(bundle.edlPath);
    if (!edlFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return failed(QStringLiteral("could not write %1").arg(bundle.edlPath));
    edlFile.write(edl.toUtf8());
    edlFile.close();

    bundle.ok = true;
    return bundle;
}

QJsonObject exportPayload(const ExportBundle &bundle, const QString &format)
{
    if (bundle.project.sequences.isEmpty())
        return QJsonObject();
    const Sequence &sequence = bundle.project.sequences.first();

    QJsonArray segments;
    for (const Track &track : sequence.tracks) {
        if (track.type != TrackType::Video)
            continue;
        for (const Segment &segment : track.segments) {
            if (segment.type == SegmentType::Gap)
                continue;
            const MediaAsset *asset = bundle.project.assetById(segment.assetId);
            if (!asset)
                continue;
            segments.append(QJsonObject{
                { QStringLiteral("name"), asset->name },
                { QStringLiteral("path"), asset->filePath },
                { QStringLiteral("timeline_in"), double(segment.timelineIn) },
                { QStringLiteral("timeline_out"), double(segment.timelineOut) },
                { QStringLiteral("source_in"), double(segment.sourceIn) },
                { QStringLiteral("source_out"), double(segment.sourceOut) },
                { QStringLiteral("generator"),
                  segment.type == SegmentType::Generator },
            });
        }
        break;
    }

    return QJsonObject{
        { QStringLiteral("title"), bundle.project.name },
        { QStringLiteral("fps"),
          QJsonObject{ { QStringLiteral("num"), sequence.fps.num },
                       { QStringLiteral("den"), sequence.fps.den } } },
        { QStringLiteral("width"), sequence.width },
        { QStringLiteral("height"), sequence.height },
        { QStringLiteral("out_path"), format == QStringLiteral("otio")
                                          ? bundle.otioPath
                                          : bundle.fcpxmlPath },
        { QStringLiteral("segments"), segments },
    };
}

} // namespace cutpilot::core::timeline
