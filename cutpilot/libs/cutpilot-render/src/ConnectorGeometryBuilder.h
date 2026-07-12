#pragma once

#include "NodeGeometryBuilder.h"

#include <QColor>
#include <QPointF>
#include <QVector>

namespace cutpilot::render {

// One connector to draw: world-space endpoints (output to input), the color of
// what flows, and whether the tail near the target is dashed to mark a permitted
// implicit conversion.
struct ConnectorDraw {
    QPointF from;
    QPointF to;
    QColor color;
    bool dashedTail = false;
};

// Triangulates connector curves into vertex-colored meshes in world coordinates,
// so pan and zoom are applied by the layer's camera matrix and never re-tessellate
// a curve. Curves are packed together into as few meshes as possible; a mesh is
// flushed before its vertex count could cross the 16-bit index ceiling, so a board
// of hundreds of connectors still draws as a handful of batches.
class ConnectorGeometryBuilder {
public:
    using Mesh = NodeGeometryBuilder::Mesh;

    // Flush threshold, comfortably under the 65535-vertex index ceiling: no single
    // connector's mesh can bridge the gap between the two.
    static constexpr int kChunkVertexLimit = 60000;

    // The dashed conversion tail: how far back from the target it starts, and the
    // world lengths of a dash and the gap after it.
    static constexpr qreal kTailWorldLength = 34.0;
    static constexpr qreal kDashWorldLength = 6.0;
    static constexpr qreal kGapWorldLength = 5.0;

    // Batch a set of connectors, packed in order, into ceiling-safe mesh chunks.
    QVector<Mesh> buildConnectors(const QVector<ConnectorDraw> &draws,
                                  qreal worldWidth) const;

    // One connector alone — the live wiring drag.
    Mesh buildSingle(const ConnectorDraw &draw, qreal worldWidth) const;

private:
    static void appendConnector(Mesh &mesh, const ConnectorDraw &draw, qreal width);
    static void appendPolyline(Mesh &mesh, const QVector<QPointF> &points, int first,
                               int last, qreal width, const QColor &color);
};

} // namespace cutpilot::render
