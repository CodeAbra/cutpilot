#include "cutpilot/core/timeline/EdlWriter.h"

#include <QFileInfo>
#include <QStringList>

namespace cutpilot::core::timeline {

namespace {

QString reelLabel(const MediaAsset &asset)
{
    const QString stem = QFileInfo(asset.name).completeBaseName();
    QString reel;
    for (const QChar &ch : stem) {
        if (ch.isLetterOrNumber())
            reel.append(ch.toUpper());
        if (reel.size() == 8)
            break;
    }
    return reel.isEmpty() ? QStringLiteral("AX") : reel;
}

const Track *firstVideoTrack(const Sequence &sequence)
{
    for (const Track &track : sequence.tracks) {
        if (track.type == TrackType::Video)
            return &track;
    }
    return nullptr;
}

} // namespace

QString writeEdl(const Sequence &sequence, const TimelineProject &project,
                 const QString &title)
{
    QStringList lines;
    lines << QStringLiteral("TITLE: %1").arg(title.isEmpty()
                                                 ? QStringLiteral("Untitled")
                                                 : title);
    lines << QStringLiteral("FCM: NON-DROP FRAME");

    const Track *track = firstVideoTrack(sequence);
    int event = 1;
    if (track) {
        for (const Segment &segment : track->segments) {
            if (segment.type == SegmentType::Gap)
                continue;
            const MediaAsset *asset = project.assetById(segment.assetId);
            if (!asset)
                continue;
            const QString srcIn = smpteTimecode(segment.sourceIn, sequence.fps);
            const QString srcOut = smpteTimecode(segment.sourceOut, sequence.fps);
            const QString recIn = smpteTimecode(segment.timelineIn, sequence.fps);
            const QString recOut =
                smpteTimecode(segment.timelineOut, sequence.fps);
            lines << QStringLiteral("%1  %2 V     C        %3 %4 %5 %6")
                         .arg(event, 3, 10, QLatin1Char('0'))
                         .arg(reelLabel(*asset), -8)
                         .arg(srcIn, srcOut, recIn, recOut);
            lines << QStringLiteral("* FROM CLIP NAME: %1").arg(asset->name);
            ++event;
        }
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

} // namespace cutpilot::core::timeline
