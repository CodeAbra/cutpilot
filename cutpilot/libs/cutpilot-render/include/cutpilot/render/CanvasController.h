#pragma once

#include <QObject>
#include <QPointF>

#include "cutpilot/render/CanvasCamera.h"

namespace cutpilot::render {

// The single world camera shared by every layer of the canvas. The grid item and
// the node layer both read this one object, so they share one world-to-screen
// transform and can never drift apart. Input is applied here (pan, zoom-about,
// reset); a change emits cameraChanged so every layer and the zoom readout repaint
// in step. Direct manipulation is exact — there is no smoothing in the camera.
class CanvasController : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal zoomPercent READ zoomPercent NOTIFY cameraChanged)

public:
    explicit CanvasController(QObject *parent = nullptr);

    qreal zoom() const { return m_camera.zoom; }
    QPointF panPixels() const { return m_camera.panPixels; }
    qreal zoomPercent() const { return m_camera.zoom * 100.0; }

    const CanvasCamera &camera() const { return m_camera; }

    // World-from-screen and screen-from-world at a given device pixel ratio, for
    // hit-testing and geometry building.
    QPointF worldFromScreen(const QPointF &screenPx, qreal dpr) const
    {
        return m_camera.worldFromScreen(screenPx, dpr);
    }
    QPointF screenFromWorld(const QPointF &world, qreal dpr) const
    {
        return m_camera.screenFromWorld(world, dpr);
    }

    void panByPixels(const QPointF &deltaPx);
    void zoomAbout(const QPointF &anchorPx, qreal factor, qreal dpr);
    void reset();

signals:
    void cameraChanged();

private:
    CanvasCamera m_camera;
};

} // namespace cutpilot::render
