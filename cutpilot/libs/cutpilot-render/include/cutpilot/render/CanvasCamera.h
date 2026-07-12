#pragma once

#include <QPointF>
#include <algorithm>

namespace cutpilot::render {

// The world camera for the infinite canvas. Pure value type with no Qt windowing
// dependency, so its transform math is unit-testable in isolation.
//
// Mapping: screenPx = world * zoom * dpr + panPixels, where panPixels is the world
// origin's position in physical pixels and dpr is the device pixel ratio. Direct
// manipulation is exact — there is no smoothing here.
struct CanvasCamera {
    qreal zoom = 1.0;          // world-to-screen scale (1.0 == 100%)
    QPointF panPixels{0, 0};   // world origin offset, in physical pixels

    static constexpr qreal kMinZoom = 0.08;
    static constexpr qreal kMaxZoom = 4.0;

    QPointF screenFromWorld(const QPointF &world, qreal dpr) const
    {
        const qreal scale = zoom * dpr;
        return world * scale + panPixels;
    }

    QPointF worldFromScreen(const QPointF &screenPx, qreal dpr) const
    {
        const qreal scale = zoom * dpr;
        return (screenPx - panPixels) / scale;
    }

    // Pan by a screen-space delta given in physical pixels (1:1 tracking).
    void panByPixels(const QPointF &deltaPx)
    {
        panPixels += deltaPx;
    }

    // Zoom by a multiplicative factor about an anchor in physical pixels, keeping
    // the world point under the anchor fixed on screen. Returns true if the zoom
    // actually changed.
    bool zoomAbout(const QPointF &anchorPx, qreal factor, qreal dpr)
    {
        const qreal newZoom = std::clamp(zoom * factor, kMinZoom, kMaxZoom);
        if (newZoom == zoom)
            return false;

        const QPointF world = worldFromScreen(anchorPx, dpr);
        zoom = newZoom;
        panPixels = anchorPx - world * (zoom * dpr);
        return true;
    }

    void reset()
    {
        zoom = 1.0;
        panPixels = QPointF(0, 0);
    }
};

} // namespace cutpilot::render
