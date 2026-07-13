#include "cutpilot/core/ConnectorPath.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace cutpilot::core {

namespace {

// Horizontal tangent reach in world units. The floor keeps a short link visibly
// curved; the ceiling stops a cross-canvas link from ballooning.
constexpr qreal kMinTangent = 48.0;
constexpr qreal kMaxTangent = 240.0;

// World units of curve per polyline segment, and the segment-count clamp.
constexpr qreal kWorldPerSegment = 18.0;
constexpr int kMinSegments = 12;
constexpr int kMaxSegments = 48;

qreal distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

} // namespace

std::array<QPointF, 4> connectorControlPoints(const QPointF &from, const QPointF &to)
{
    const qreal dx = to.x() - from.x();
    const qreal dy = to.y() - from.y();

    qreal reach = qAbs(dx) * 0.5;
    if (dx < 0.0) {
        // A backward target: reach past the horizontal gap and lean on the vertical
        // offset so the S-loop clears both cards.
        reach = qAbs(dx) * 0.6 + qAbs(dy) * 0.2;
    }
    reach = qBound(kMinTangent, reach, kMaxTangent);

    return { from, from + QPointF(reach, 0.0), to - QPointF(reach, 0.0), to };
}

QVector<QPointF> sampleConnector(const QPointF &from, const QPointF &to)
{
    const std::array<QPointF, 4> cp = connectorControlPoints(from, to);

    // The control polygon length bounds the arc length from above, which is a good
    // enough size cue for picking a segment count.
    const qreal polygonLength =
        distance(cp[0], cp[1]) + distance(cp[1], cp[2]) + distance(cp[2], cp[3]);
    const int segments =
        qBound(kMinSegments, int(polygonLength / kWorldPerSegment), kMaxSegments);

    QVector<QPointF> points;
    points.reserve(segments + 1);
    points.push_back(from);
    for (int i = 1; i < segments; ++i) {
        const qreal t = qreal(i) / qreal(segments);
        const qreal u = 1.0 - t;
        const QPointF p = u * u * u * cp[0] + 3.0 * u * u * t * cp[1]
            + 3.0 * u * t * t * cp[2] + t * t * t * cp[3];
        points.push_back(p);
    }
    points.push_back(to);
    return points;
}

qreal connectorDistance(const QPointF &from, const QPointF &to, const QPointF &point)
{
    const QVector<QPointF> polyline = sampleConnector(from, to);
    qreal best = distance(polyline.first(), point);
    for (int i = 0; i + 1 < polyline.size(); ++i) {
        const QPointF a = polyline[i];
        const QPointF b = polyline[i + 1];
        const QPointF ab = b - a;
        const qreal lengthSq = ab.x() * ab.x() + ab.y() * ab.y();
        QPointF nearest = a;
        if (lengthSq > 1e-12) {
            const qreal t = std::clamp(
                ((point.x() - a.x()) * ab.x() + (point.y() - a.y()) * ab.y())
                    / lengthSq,
                0.0, 1.0);
            nearest = a + ab * t;
        }
        best = std::min(best, distance(nearest, point));
    }
    return best;
}

QRectF connectorBounds(const QPointF &from, const QPointF &to, qreal pad)
{
    const std::array<QPointF, 4> cp = connectorControlPoints(from, to);
    qreal left = cp[0].x(), right = cp[0].x(), top = cp[0].y(), bottom = cp[0].y();
    for (const QPointF &p : cp) {
        left = std::min(left, p.x());
        right = std::max(right, p.x());
        top = std::min(top, p.y());
        bottom = std::max(bottom, p.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom))
        .adjusted(-pad, -pad, pad, pad);
}

} // namespace cutpilot::core
