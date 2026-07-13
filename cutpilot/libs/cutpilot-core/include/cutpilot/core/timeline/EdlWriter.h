#pragma once

#include "cutpilot/core/timeline/Timeline.h"

namespace cutpilot::core::timeline {

// A CMX 3600 edit decision list of the sequence's first video track: a TITLE
// and FCM header, one numbered cut event per clip with source and record
// timecodes at the sequence rate, and a FROM CLIP NAME comment naming each
// event's media file. Gap segments advance the record timecode without
// producing an event. Reels are the first eight alphanumeric characters of
// the asset name, uppercased, AX when nothing survives.
QString writeEdl(const Sequence &sequence, const TimelineProject &project,
                 const QString &title);

} // namespace cutpilot::core::timeline
