#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
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

    // Set an absolute zoom about an anchor in physical pixels, keeping the world
    // point under the anchor fixed on screen. Returns true if the zoom changed.
    bool setZoomAbout(const QPointF &anchorPx, qreal newZoom, qreal dpr)
    {
        const qreal clamped = std::clamp(newZoom, kMinZoom, kMaxZoom);
        if (clamped == zoom)
            return false;
        const QPointF world = worldFromScreen(anchorPx, dpr);
        zoom = clamped;
        panPixels = anchorPx - world * (zoom * dpr);
        return true;
    }

    // Pan so the given world point lands at the viewport center, keeping the zoom.
    // The viewport size is in physical pixels.
    void centerOn(const QPointF &world, const QSizeF &viewportPx, qreal dpr)
    {
        const QPointF centre(viewportPx.width() / 2.0, viewportPx.height() / 2.0);
        panPixels = centre - world * (zoom * dpr);
    }

    // Frame a world rect in the viewport with a fractional margin on every side,
    // clamped to the zoom range. A degenerate rect only recenters.
    void fitRect(const QRectF &world, const QSizeF &viewportPx, qreal dpr,
                 qreal marginFrac = 0.08)
    {
        if (viewportPx.isEmpty())
            return;
        if (world.width() > 1e-9 && world.height() > 1e-9) {
            const qreal usableW = viewportPx.width() * (1.0 - 2.0 * marginFrac);
            const qreal usableH = viewportPx.height() * (1.0 - 2.0 * marginFrac);
            const qreal fitted = std::min(usableW / (world.width() * dpr),
                                          usableH / (world.height() * dpr));
            zoom = std::clamp(fitted, kMinZoom, kMaxZoom);
        }
        centerOn(world.center(), viewportPx, dpr);
    }

    void reset()
    {
        zoom = 1.0;
        panPixels = QPointF(0, 0);
    }
};

} // namespace cutpilot::render
