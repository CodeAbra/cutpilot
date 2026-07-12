#pragma once

#include "cutpilot/core/Node.h"

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QVector>

#include <cstdint>

namespace cutpilot::core {

// A uniform spatial-hash grid over world space. A node registers in every fixed cell
// its world bounds overlap; queries gather candidates from the overlapped cells and
// exact-test against the stored bounds, so a mere cell co-residency never yields a
// false hit. Cell keys pack the two signed cell coordinates into one 64-bit value, so
// negative and far-apart coordinates map to distinct buckets. This is the structure the
// canvas culls and picks through instead of scanning every node.
class SpatialIndex {
public:
    // Fixed cell edge in world units, on the order of a node's larger side.
    static constexpr qreal kDefaultCellSize = 320.0;

    explicit SpatialIndex(qreal cellSize = kDefaultCellSize);

    void clear();

    // Register a node and its world bounds. Replaces any existing entry for the id.
    void insert(int id, const QRectF &worldBounds);
    void remove(int id);
    void update(int id, const QRectF &worldBounds);

    // Reconstruct the whole index from a node list.
    void rebuild(const QVector<Node> &nodes);

    // Unique ids whose stored bounds intersect the rect (the culling query).
    QVector<int> queryRect(const QRectF &worldRect) const;

    // Ids whose stored bounds contain the world point (narrows picking).
    QVector<int> queryPoint(const QPointF &world) const;

    int count() const { return int(m_bounds.size()); }

private:
    int cellCoord(qreal world) const;
    static quint64 cellKey(int cx, int cy);

    // Invoke fn(cx, cy) for every cell the rect overlaps.
    template <typename Fn>
    void forEachCell(const QRectF &rect, Fn &&fn) const;

    qreal m_cellSize;
    QHash<int, QRectF> m_bounds;
    QHash<quint64, QVector<int>> m_cells;
};

// The world rectangle a viewport of the given pixel size covers under a camera whose
// mapping is screenPixel = world * scale + translation. Pure value math with no GUI
// type, so the render layer and the tests share one culling computation.
QRectF viewportWorldRect(qreal scale, const QPointF &translation,
                         const QSizeF &viewportSizePx);

// The ids visible in that viewport: the index's rect query over viewportWorldRect.
QVector<int> visibleIds(const SpatialIndex &index, qreal scale,
                        const QPointF &translation, const QSizeF &viewportSizePx);

} // namespace cutpilot::core
