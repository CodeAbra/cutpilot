#include "ConnectorGeometryBuilder.h"

#include "cutpilot/core/ConnectorPath.h"

#include <QtGlobal>

#include <cmath>

namespace cutpilot::render {

namespace {

// Headroom left below the chunk limit when deciding to flush: no single connector
// (solid run plus dashed tail) comes close to this many vertices.
constexpr int kFlushMargin = 512;

qreal distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(b.x() - a.x(), b.y() - a.y());
}

// The point at arc distance s along the polyline, via the per-point cumulative
// lengths. s is clamped to the polyline's span.
QPointF pointAtArc(const QVector<QPointF> &points, const QVector<qreal> &cumulative,
                   qreal s)
{
    if (s <= 0.0)
        return points.first();
    const qreal total = cumulative.last();
    if (s >= total)
        return points.last();
    int i = 1;
    while (cumulative[i] < s)
        ++i;
    const qreal segment = cumulative[i] - cumulative[i - 1];
    const qreal t = segment > 0.0 ? (s - cumulative[i - 1]) / segment : 0.0;
    return points[i - 1] + t * (points[i] - points[i - 1]);
}

} // namespace

void ConnectorGeometryBuilder::appendPolyline(Mesh &mesh, const QVector<QPointF> &points,
                                              int first, int last, qreal width,
                                              const QColor &color)
{
    for (int i = first; i < last; ++i)
        NodeGeometryBuilder::appendLineQuad(mesh, points[i], points[i + 1], width, color);
}

void ConnectorGeometryBuilder::appendConnector(Mesh &mesh, const ConnectorDraw &draw,
                                               qreal width)
{
    const QVector<QPointF> points = core::sampleConnector(draw.from, draw.to);
    if (points.size() < 2)
        return;

    if (!draw.dashedTail) {
        appendPolyline(mesh, points, 0, points.size() - 1, width, draw.color);
        return;
    }

    QVector<qreal> cumulative(points.size());
    cumulative[0] = 0.0;
    for (int i = 1; i < points.size(); ++i)
        cumulative[i] = cumulative[i - 1] + distance(points[i - 1], points[i]);
    const qreal total = cumulative.last();

    // The solid head runs up to the tail start; the tail is re-walked by arc length
    // because the dash period is finer than the curve's sampling.
    const qreal tailStart = qMax(0.0, total - kTailWorldLength);

    QVector<QPointF> head;
    head.reserve(points.size());
    head.push_back(points.first());
    for (int i = 1; i < points.size() && cumulative[i] < tailStart; ++i)
        head.push_back(points[i]);
    head.push_back(pointAtArc(points, cumulative, tailStart));
    appendPolyline(mesh, head, 0, head.size() - 1, width, draw.color);

    for (qreal s = tailStart; s < total; s += kDashWorldLength + kGapWorldLength) {
        const QPointF a = pointAtArc(points, cumulative, s);
        const QPointF b = pointAtArc(points, cumulative, qMin(s + kDashWorldLength, total));
        NodeGeometryBuilder::appendLineQuad(mesh, a, b, width, draw.color);
    }
}

QVector<ConnectorGeometryBuilder::Mesh>
ConnectorGeometryBuilder::buildConnectors(const QVector<ConnectorDraw> &draws,
                                          qreal worldWidth) const
{
    QVector<Mesh> chunks;
    Mesh current;
    for (const ConnectorDraw &draw : draws) {
        appendConnector(current, draw, worldWidth);
        if (current.vertices.size() > kChunkVertexLimit - kFlushMargin) {
            chunks.push_back(std::move(current));
            current = Mesh{};
        }
    }
    if (!current.vertices.isEmpty())
        chunks.push_back(std::move(current));

    for (const Mesh &chunk : chunks)
        Q_ASSERT(chunk.vertices.size() <= 0xFFFF);
    return chunks;
}

ConnectorGeometryBuilder::Mesh
ConnectorGeometryBuilder::buildSingle(const ConnectorDraw &draw, qreal worldWidth) const
{
    Mesh mesh;
    appendConnector(mesh, draw, worldWidth);
    Q_ASSERT(mesh.vertices.size() <= 0xFFFF);
    return mesh;
}

} // namespace cutpilot::render
