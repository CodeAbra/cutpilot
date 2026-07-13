#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <algorithm>

namespace cutpilot::render {

// The uniform world-to-minimap mapping: the whole board framed inside the minimap
// with padding, aspect preserved and centered. A pure value type with no windowing
// dependency, mirroring CanvasCamera, so the mapping is unit-testable exactly.
//
// Mapping: mini = world * scale + offset, in the minimap's logical pixels.
struct MinimapProjection {
    qreal scale = 1.0;
    QPointF offset{0, 0};

    // The fallback half-extent framed around a degenerate board (empty, or a
    // single point), so the projection always stays invertible.
    static constexpr qreal kFallbackHalfExtent = 500.0;

    static MinimapProjection fit(const QRectF &worldBounds, const QSizeF &minimapSize,
                                 qreal padding)
    {
        QRectF bounds = worldBounds;
        if (bounds.width() <= 1e-9 || bounds.height() <= 1e-9) {
            const QPointF centre = bounds.center();
            bounds = QRectF(centre.x() - kFallbackHalfExtent,
                            centre.y() - kFallbackHalfExtent,
                            kFallbackHalfExtent * 2.0, kFallbackHalfExtent * 2.0);
        }

        const qreal innerW = std::max(1.0, minimapSize.width() - 2.0 * padding);
        const qreal innerH = std::max(1.0, minimapSize.height() - 2.0 * padding);

        MinimapProjection projection;
        projection.scale =
            std::min(innerW / bounds.width(), innerH / bounds.height());
        const QPointF miniCentre(minimapSize.width() / 2.0,
                                 minimapSize.height() / 2.0);
        projection.offset = miniCentre - bounds.center() * projection.scale;
        return projection;
    }

    QPointF miniFromWorld(const QPointF &world) const
    {
        return world * scale + offset;
    }

    QPointF worldFromMini(const QPointF &mini) const
    {
        return (mini - offset) / scale;
    }

    QRectF miniFromWorld(const QRectF &world) const
    {
        return QRectF(miniFromWorld(world.topLeft()), world.size() * scale);
    }
};

} // namespace cutpilot::render
