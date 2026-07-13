#include "cutpilot/core/timeline/Timeline.h"

#include <QJsonArray>
#include <QSet>

namespace cutpilot::core::timeline {

namespace {

QString segmentTypeName(SegmentType type)
{
    switch (type) {
    case SegmentType::Clip:
        return QStringLiteral("clip");
    case SegmentType::Gap:
        return QStringLiteral("gap");
    case SegmentType::Generator:
        return QStringLiteral("generator");
    }
    return QStringLiteral("clip");
}

SegmentType segmentTypeFromName(const QString &name)
{
    if (name == QStringLiteral("gap"))
        return SegmentType::Gap;
    if (name == QStringLiteral("generator"))
        return SegmentType::Generator;
    return SegmentType::Clip;
}

QString trackTypeName(TrackType type)
{
    return type == TrackType::Audio ? QStringLiteral("audio")
                                    : QStringLiteral("video");
}

TrackType trackTypeFromName(const QString &name)
{
    return name == QStringLiteral("audio") ? TrackType::Audio : TrackType::Video;
}

QJsonObject fpsToJson(const Fps &fps)
{
    return QJsonObject{ { QStringLiteral("num"), fps.num },
                        { QStringLiteral("den"), fps.den } };
}

Fps fpsFromJson(const QJsonObject &json)
{
    Fps fps;
    fps.num = json.value(QStringLiteral("num")).toInt(24);
    fps.den = json.value(QStringLiteral("den")).toInt(1);
    return fps;
}

} // namespace

QString smpteTimecode(qint64 frames, const Fps &fps)
{
    const qint64 rate = qMax(1, fps.nominalRate());
    qint64 remaining = qMax<qint64>(0, frames);
    const qint64 hours = remaining / (rate * 3600);
    remaining -= hours * rate * 3600;
    const qint64 minutes = remaining / (rate * 60);
    remaining -= minutes * rate * 60;
    const qint64 seconds = remaining / rate;
    const qint64 frame = remaining - seconds * rate;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frame, 2, 10, QLatin1Char('0'));
}

QString MediaAsset::validate() const
{
    if (id.trimmed().isEmpty())
        return QStringLiteral("media asset id must not be empty");
    if (filePath.trimmed().isEmpty())
        return QStringLiteral("media asset file path must not be empty");
    if (!fps.isValid())
        return QStringLiteral("media asset frame rate must be positive");
    if (width < 0 || height < 0)
        return QStringLiteral("media asset dimensions must not be negative");
    if (durationFrames < 0)
        return QStringLiteral("media asset duration must not be negative");
    return QString();
}

QJsonObject MediaAsset::toJson() const
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("filePath"), filePath },
        { QStringLiteral("name"), name },
        { QStringLiteral("width"), width },
        { QStringLiteral("height"), height },
        { QStringLiteral("fps"), fpsToJson(fps) },
        { QStringLiteral("durationFrames"), double(durationFrames) },
    };
}

MediaAsset MediaAsset::fromJson(const QJsonObject &json)
{
    MediaAsset asset;
    asset.id = json.value(QStringLiteral("id")).toString();
    asset.filePath = json.value(QStringLiteral("filePath")).toString();
    asset.name = json.value(QStringLiteral("name")).toString();
    asset.width = json.value(QStringLiteral("width")).toInt();
    asset.height = json.value(QStringLiteral("height")).toInt();
    asset.fps = fpsFromJson(json.value(QStringLiteral("fps")).toObject());
    asset.durationFrames =
        qint64(json.value(QStringLiteral("durationFrames")).toDouble());
    return asset;
}

QString Segment::validate() const
{
    if (id.trimmed().isEmpty())
        return QStringLiteral("segment id must not be empty");
    if (timelineOut <= timelineIn)
        return QStringLiteral("segment timeline out must be greater than in");
    if (timelineIn < 0)
        return QStringLiteral("segment timeline in must not be negative");
    if (type == SegmentType::Gap) {
        if (!assetId.isEmpty())
            return QStringLiteral("gap segment must not reference an asset");
        return QString();
    }
    if (assetId.trimmed().isEmpty())
        return QStringLiteral("segment must reference an asset");
    if (sourceIn < 0)
        return QStringLiteral("segment source in must not be negative");
    if (sourceOut <= sourceIn)
        return QStringLiteral("segment source out must be greater than in");
    return QString();
}

QJsonObject Segment::toJson() const
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("type"), segmentTypeName(type) },
        { QStringLiteral("assetId"), assetId },
        { QStringLiteral("timelineIn"), double(timelineIn) },
        { QStringLiteral("timelineOut"), double(timelineOut) },
        { QStringLiteral("sourceIn"), double(sourceIn) },
        { QStringLiteral("sourceOut"), double(sourceOut) },
    };
}

Segment Segment::fromJson(const QJsonObject &json)
{
    Segment segment;
    segment.id = json.value(QStringLiteral("id")).toString();
    segment.type =
        segmentTypeFromName(json.value(QStringLiteral("type")).toString());
    segment.assetId = json.value(QStringLiteral("assetId")).toString();
    segment.timelineIn = qint64(json.value(QStringLiteral("timelineIn")).toDouble());
    segment.timelineOut =
        qint64(json.value(QStringLiteral("timelineOut")).toDouble());
    segment.sourceIn = qint64(json.value(QStringLiteral("sourceIn")).toDouble());
    segment.sourceOut = qint64(json.value(QStringLiteral("sourceOut")).toDouble());
    return segment;
}

QString Track::validate() const
{
    qint64 cursor = 0;
    for (const Segment &segment : segments) {
        const QString error = segment.validate();
        if (!error.isEmpty())
            return error;
        if (segment.timelineIn < cursor)
            return QStringLiteral(
                "track segments must be ordered and non-overlapping");
        cursor = segment.timelineOut;
    }
    return QString();
}

QJsonObject Track::toJson() const
{
    QJsonArray segmentArray;
    for (const Segment &segment : segments)
        segmentArray.append(segment.toJson());
    return QJsonObject{
        { QStringLiteral("name"), name },
        { QStringLiteral("type"), trackTypeName(type) },
        { QStringLiteral("segments"), segmentArray },
    };
}

Track Track::fromJson(const QJsonObject &json)
{
    Track track;
    track.name = json.value(QStringLiteral("name")).toString();
    track.type = trackTypeFromName(json.value(QStringLiteral("type")).toString());
    const QJsonArray segmentArray =
        json.value(QStringLiteral("segments")).toArray();
    track.segments.reserve(segmentArray.size());
    for (const QJsonValue &value : segmentArray)
        track.segments.append(Segment::fromJson(value.toObject()));
    return track;
}

qint64 Sequence::durationFrames() const
{
    qint64 duration = 0;
    for (const Track &track : tracks) {
        if (!track.segments.isEmpty())
            duration = qMax(duration, track.segments.last().timelineOut);
    }
    return duration;
}

QString Sequence::validate() const
{
    if (name.trimmed().isEmpty())
        return QStringLiteral("sequence name must not be empty");
    if (!fps.isValid())
        return QStringLiteral("sequence frame rate must be positive");
    if (width <= 0 || height <= 0)
        return QStringLiteral("sequence dimensions must be positive");
    for (const Track &track : tracks) {
        const QString error = track.validate();
        if (!error.isEmpty())
            return error;
    }
    return QString();
}

QJsonObject Sequence::toJson() const
{
    QJsonArray trackArray;
    for (const Track &track : tracks)
        trackArray.append(track.toJson());
    return QJsonObject{
        { QStringLiteral("name"), name },
        { QStringLiteral("fps"), fpsToJson(fps) },
        { QStringLiteral("width"), width },
        { QStringLiteral("height"), height },
        { QStringLiteral("tracks"), trackArray },
    };
}

Sequence Sequence::fromJson(const QJsonObject &json)
{
    Sequence sequence;
    sequence.name = json.value(QStringLiteral("name")).toString();
    sequence.fps = fpsFromJson(json.value(QStringLiteral("fps")).toObject());
    sequence.width = json.value(QStringLiteral("width")).toInt(1920);
    sequence.height = json.value(QStringLiteral("height")).toInt(1080);
    const QJsonArray trackArray = json.value(QStringLiteral("tracks")).toArray();
    sequence.tracks.reserve(trackArray.size());
    for (const QJsonValue &value : trackArray)
        sequence.tracks.append(Track::fromJson(value.toObject()));
    return sequence;
}

const MediaAsset *TimelineProject::assetById(const QString &id) const
{
    for (const MediaAsset &asset : assets) {
        if (asset.id == id)
            return &asset;
    }
    return nullptr;
}

QString TimelineProject::validate() const
{
    if (name.trimmed().isEmpty())
        return QStringLiteral("project name must not be empty");
    QSet<QString> assetIds;
    for (const MediaAsset &asset : assets) {
        const QString error = asset.validate();
        if (!error.isEmpty())
            return error;
        if (assetIds.contains(asset.id))
            return QStringLiteral("asset ids must be unique");
        assetIds.insert(asset.id);
    }
    for (const Sequence &sequence : sequences) {
        const QString error = sequence.validate();
        if (!error.isEmpty())
            return error;
        for (const Track &track : sequence.tracks) {
            for (const Segment &segment : track.segments) {
                if (segment.type != SegmentType::Gap
                    && !assetIds.contains(segment.assetId))
                    return QStringLiteral(
                        "segment references a missing asset");
            }
        }
    }
    return QString();
}

QJsonObject TimelineProject::toJson() const
{
    QJsonArray assetArray;
    for (const MediaAsset &asset : assets)
        assetArray.append(asset.toJson());
    QJsonArray sequenceArray;
    for (const Sequence &sequence : sequences)
        sequenceArray.append(sequence.toJson());
    return QJsonObject{
        { QStringLiteral("name"), name },
        { QStringLiteral("assets"), assetArray },
        { QStringLiteral("sequences"), sequenceArray },
    };
}

TimelineProject TimelineProject::fromJson(const QJsonObject &json)
{
    TimelineProject project;
    project.name = json.value(QStringLiteral("name")).toString();
    const QJsonArray assetArray = json.value(QStringLiteral("assets")).toArray();
    project.assets.reserve(assetArray.size());
    for (const QJsonValue &value : assetArray)
        project.assets.append(MediaAsset::fromJson(value.toObject()));
    const QJsonArray sequenceArray =
        json.value(QStringLiteral("sequences")).toArray();
    project.sequences.reserve(sequenceArray.size());
    for (const QJsonValue &value : sequenceArray)
        project.sequences.append(Sequence::fromJson(value.toObject()));
    return project;
}

} // namespace cutpilot::core::timeline
