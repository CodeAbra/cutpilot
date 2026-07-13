#pragma once

#include "cutpilot/core/timeline/Timeline.h"

namespace cutpilot::core::timeline {

// Persistence for the project model. Saves are atomic — the file either
// keeps its previous contents or holds the complete new document — and a
// load validates before handing the project back.
bool saveProject(const TimelineProject &project, const QString &path,
                 QString *error = nullptr);
bool loadProject(const QString &path, TimelineProject *project,
                 QString *error = nullptr);

// Land a produced media file in the project as a Generator segment appended
// to the first video track of the first sequence, creating the sequence and
// track when the project has none. Returns the new segment's id. A colliding
// asset id is suffixed until unique; duration falls back to four seconds at
// the sequence rate when the asset carries none.
QString appendGeneratorSegment(TimelineProject &project,
                               const MediaAsset &asset);

} // namespace cutpilot::core::timeline
