#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace cutpilot::core::timeline {

// Frames per second as an exact ratio, so rational rates like 24000/1001
// never accumulate float drift across an edit. All timeline positions are
// integer frame counts at the owning sequence's rate.
struct Fps {
    int num = 24;
    int den = 1;

    bool isValid() const { return num > 0 && den > 0; }

    // The nominal counting rate for timecode display: 24000/1001 counts as 24.
    int nominalRate() const { return den > 0 ? (num + den / 2) / den : 0; }

    bool operator==(const Fps &other) const = default;
};

// A frame count rendered as non-drop SMPTE timecode (HH:MM:SS:FF) at the
// rate's nominal counting speed. Negative frame counts clamp to zero.
QString smpteTimecode(qint64 frames, const Fps &fps);

// One media file the timeline references. Width/height/duration may be zero
// when unknown (a still whose header was never read, a video never probed);
// zero means unknown, never invalid.
struct MediaAsset {
    QString id;
    QString filePath;
    QString name;
    int width = 0;
    int height = 0;
    Fps fps;
    qint64 durationFrames = 0;

    // Empty when valid; otherwise the reason.
    QString validate() const;

    QJsonObject toJson() const;
    static MediaAsset fromJson(const QJsonObject &json);

    bool operator==(const MediaAsset &other) const = default;
};

// What occupies a span of a track: media brought in from disk, silence/black,
// or produced media (a generation result or a composite render).
enum class SegmentType {
    Clip,
    Gap,
    Generator
};

// A half-open span [timelineIn, timelineOut) on a track, cutting the source
// span [sourceIn, sourceOut) out of its asset. Gap segments reference no
// asset.
struct Segment {
    QString id;
    SegmentType type = SegmentType::Clip;
    QString assetId;
    qint64 timelineIn = 0;
    qint64 timelineOut = 0;
    qint64 sourceIn = 0;
    qint64 sourceOut = 0;

    qint64 durationFrames() const { return timelineOut - timelineIn; }

    QString validate() const;

    QJsonObject toJson() const;
    static Segment fromJson(const QJsonObject &json);

    bool operator==(const Segment &other) const = default;
};

enum class TrackType {
    Video,
    Audio
};

// Segments in ascending, non-overlapping timeline order.
struct Track {
    QString name;
    TrackType type = TrackType::Video;
    QVector<Segment> segments;

    QString validate() const;

    QJsonObject toJson() const;
    static Track fromJson(const QJsonObject &json);

    bool operator==(const Track &other) const = default;
};

struct Sequence {
    QString name;
    Fps fps;
    int width = 1920;
    int height = 1080;
    QVector<Track> tracks;

    // The end of the last segment across every track.
    qint64 durationFrames() const;

    QString validate() const;

    QJsonObject toJson() const;
    static Sequence fromJson(const QJsonObject &json);

    bool operator==(const Sequence &other) const = default;
};

// The project model the canvas exports into: the asset table plus the
// sequences cut from it. Every segment's assetId must resolve.
struct TimelineProject {
    QString name;
    QVector<MediaAsset> assets;
    QVector<Sequence> sequences;

    const MediaAsset *assetById(const QString &id) const;

    QString validate() const;

    QJsonObject toJson() const;
    static TimelineProject fromJson(const QJsonObject &json);

    bool operator==(const TimelineProject &other) const = default;
};

} // namespace cutpilot::core::timeline
