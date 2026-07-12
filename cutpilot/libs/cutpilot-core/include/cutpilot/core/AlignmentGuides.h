#pragma once

#include <QRectF>
#include <QVector>

namespace cutpilot::core {

enum class GuideAxis {
    Vertical,   // a vertical line at a fixed world x
    Horizontal  // a horizontal line at a fixed world y
};

// One alignment guide: an axis, the world coordinate of the line, and the world span it
// covers along the other axis (the union extent of the moving rect and the matched
// neighbour), so the render layer can draw a finite hairline rather than a full-field
// line.
struct AlignmentGuide {
    GuideAxis axis = GuideAxis::Vertical;
    qreal coordinate = 0.0;
    qreal spanStart = 0.0;
    qreal spanEnd = 0.0;
};

// Compare the moving reference's left/centre/right and top/middle/bottom against the
// same features of each neighbour, and return one guide per feature pair within
// worldThreshold. A multi-node drag passes the selection's bounding box as the moving
// rect; the comparison is identical. Pure value math with no GUI type, so the guide
// logic is headless-testable and the render layer only meshes the returned lines.
QVector<AlignmentGuide> computeAlignmentGuides(const QRectF &movingRect,
                                               const QVector<QRectF> &neighbourRects,
                                               qreal worldThreshold);

} // namespace cutpilot::core
