#include "cutpilot/core/timeline/ProjectStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace cutpilot::core::timeline {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

} // namespace

bool saveProject(const TimelineProject &project, const QString &path,
                 QString *error)
{
    const QString invalid = project.validate();
    if (!invalid.isEmpty()) {
        setError(error, invalid);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, file.errorString());
        return false;
    }
    file.write(QJsonDocument(project.toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool loadProject(const QString &path, TimelineProject *project, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, parseError.errorString());
        return false;
    }
    if (!document.isObject()) {
        setError(error, QStringLiteral("project file holds no object"));
        return false;
    }
    TimelineProject loaded = TimelineProject::fromJson(document.object());
    const QString invalid = loaded.validate();
    if (!invalid.isEmpty()) {
        setError(error, invalid);
        return false;
    }
    if (project)
        *project = loaded;
    return true;
}

QString appendGeneratorSegment(TimelineProject &project,
                               const MediaAsset &asset)
{
    if (project.name.isEmpty())
        project.name = QStringLiteral("CutPilot");
    if (project.sequences.isEmpty()) {
        Sequence sequence;
        sequence.name = QStringLiteral("Timeline");
        project.sequences.append(sequence);
    }
    Sequence &sequence = project.sequences.first();

    Track *video = nullptr;
    for (Track &track : sequence.tracks) {
        if (track.type == TrackType::Video) {
            video = &track;
            break;
        }
    }
    if (!video) {
        Track track;
        track.name = QStringLiteral("V1");
        track.type = TrackType::Video;
        sequence.tracks.append(track);
        video = &sequence.tracks.last();
    }

    MediaAsset stored = asset;
    if (stored.durationFrames <= 0)
        stored.durationFrames = qint64(4) * qMax(1, sequence.fps.nominalRate());
    const QString baseId = stored.id.isEmpty() ? QStringLiteral("asset")
                                               : stored.id;
    stored.id = baseId;
    for (int suffix = 2; project.assetById(stored.id) != nullptr; ++suffix)
        stored.id = QStringLiteral("%1-%2").arg(baseId).arg(suffix);
    project.assets.append(stored);

    Segment segment;
    segment.id = QStringLiteral("segment-%1").arg(stored.id);
    segment.type = SegmentType::Generator;
    segment.assetId = stored.id;
    segment.timelineIn = sequence.durationFrames();
    segment.timelineOut = segment.timelineIn + stored.durationFrames;
    segment.sourceIn = 0;
    segment.sourceOut = stored.durationFrames;
    video->segments.append(segment);
    return segment.id;
}

} // namespace cutpilot::core::timeline
