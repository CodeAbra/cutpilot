#include "cutpilot/core/SpatialIndex.h"

#include <QSet>

#include <cmath>

namespace cutpilot::core {

SpatialIndex::SpatialIndex(qreal cellSize)
    : m_cellSize(cellSize > 0.0 ? cellSize : kDefaultCellSize)
{
}

void SpatialIndex::clear()
{
    m_bounds.clear();
    m_cells.clear();
}

int SpatialIndex::cellCoord(qreal world) const
{
    // Floor division so negative coordinates map to the correct cell.
    return int(std::floor(world / m_cellSize));
}

quint64 SpatialIndex::cellKey(int cx, int cy)
{
    // Pack two signed 32-bit cell coordinates into one 64-bit key. The unsigned casts
    // preserve the bit pattern, so distinct cells never share a key.
    return (quint64(quint32(cx)) << 32) | quint64(quint32(cy));
}

template <typename Fn>
void SpatialIndex::forEachCell(const QRectF &rect, Fn &&fn) const
{
    const int minX = cellCoord(rect.left());
    const int maxX = cellCoord(rect.right());
    const int minY = cellCoord(rect.top());
    const int maxY = cellCoord(rect.bottom());
    for (int cy = minY; cy <= maxY; ++cy) {
        for (int cx = minX; cx <= maxX; ++cx)
            fn(cx, cy);
    }
}

void SpatialIndex::insert(int id, const QRectF &worldBounds)
{
    remove(id);
    m_bounds.insert(id, worldBounds);
    forEachCell(worldBounds,
                [&](int cx, int cy) { m_cells[cellKey(cx, cy)].push_back(id); });
}

void SpatialIndex::remove(int id)
{
    const auto it = m_bounds.constFind(id);
    if (it == m_bounds.cend())
        return;
    const QRectF bounds = it.value();
    forEachCell(bounds, [&](int cx, int cy) {
        const quint64 key = cellKey(cx, cy);
        const auto cell = m_cells.find(key);
        if (cell == m_cells.end())
            return;
        cell.value().removeAll(id);
        if (cell.value().isEmpty())
            m_cells.erase(cell);
    });
    m_bounds.erase(m_bounds.find(id));
}

void SpatialIndex::update(int id, const QRectF &worldBounds)
{
    insert(id, worldBounds);
}

void SpatialIndex::rebuild(const QVector<Node> &nodes)
{
    clear();
    for (const Node &n : nodes)
        insert(n.id, n.worldRect());
}

QVector<int> SpatialIndex::queryRect(const QRectF &worldRect) const
{
    QVector<int> result;
    QSet<int> seen;
    forEachCell(worldRect, [&](int cx, int cy) {
        const auto cell = m_cells.constFind(cellKey(cx, cy));
        if (cell == m_cells.cend())
            return;
        for (int id : cell.value()) {
            if (seen.contains(id))
                continue;
            const auto b = m_bounds.constFind(id);
            if (b != m_bounds.cend() && b.value().intersects(worldRect)) {
                seen.insert(id);
                result.push_back(id);
            }
        }
    });
    return result;
}

QVector<int> SpatialIndex::queryPoint(const QPointF &world) const
{
    QVector<int> result;
    const auto cell = m_cells.constFind(cellKey(cellCoord(world.x()), cellCoord(world.y())));
    if (cell == m_cells.cend())
        return result;
    for (int id : cell.value()) {
        const auto b = m_bounds.constFind(id);
        if (b != m_bounds.cend() && b.value().contains(world))
            result.push_back(id);
    }
    return result;
}

QRectF viewportWorldRect(qreal scale, const QPointF &translation,
                         const QSizeF &viewportSizePx)
{
    const qreal s = scale != 0.0 ? scale : 1.0;
    const QPointF topLeft = (QPointF(0, 0) - translation) / s;
    const QPointF bottomRight =
        (QPointF(viewportSizePx.width(), viewportSizePx.height()) - translation) / s;
    return QRectF(topLeft, bottomRight);
}

QVector<int> visibleIds(const SpatialIndex &index, qreal scale,
                        const QPointF &translation, const QSizeF &viewportSizePx)
{
    return index.queryRect(viewportWorldRect(scale, translation, viewportSizePx));
}

} // namespace cutpilot::core
