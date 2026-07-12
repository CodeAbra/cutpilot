#pragma once

#include <QPointF>
#include <QRectF>
#include <QVector>

#include <array>

namespace cutpilot::core {

// The cubic Bezier a connector follows from an output port to an input port. The
// curve leaves the output heading right and enters the input heading right, so it
// reads cleanly wherever the two ends sit; when the target lies left of its source
// the tangents lengthen to swing the curve around in a soft loop.
std::array<QPointF, 4> connectorControlPoints(const QPointF &from, const QPointF &to);

// The curve sampled as a polyline. The segment count grows with the span and is
// clamped, so short links stay cheap and long links stay smooth. The first and
// last points equal from and to exactly.
QVector<QPointF> sampleConnector(const QPointF &from, const QPointF &to);

// A rect guaranteed to contain the whole curve (its control hull) grown by pad on
// every side. Used for connector culling against the viewport.
QRectF connectorBounds(const QPointF &from, const QPointF &to, qreal pad);

} // namespace cutpilot::core
