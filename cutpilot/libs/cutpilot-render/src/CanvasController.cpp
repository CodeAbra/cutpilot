#include "cutpilot/render/CanvasController.h"

namespace cutpilot::render {

CanvasController::CanvasController(QObject *parent)
    : QObject(parent)
{
}

void CanvasController::panByPixels(const QPointF &deltaPx)
{
    if (deltaPx.isNull())
        return;
    m_camera.panByPixels(deltaPx);
    emit cameraChanged();
}

void CanvasController::zoomAbout(const QPointF &anchorPx, qreal factor, qreal dpr)
{
    if (m_camera.zoomAbout(anchorPx, factor, dpr))
        emit cameraChanged();
}

void CanvasController::reset()
{
    m_camera.reset();
    emit cameraChanged();
}

void CanvasController::setZoomPercent(qreal percent, const QPointF &anchorPx,
                                      qreal dpr)
{
    if (m_camera.setZoomAbout(anchorPx, percent / 100.0, dpr))
        emit cameraChanged();
}

void CanvasController::fitWorldRect(const QRectF &world, const QSizeF &viewportPx,
                                    qreal dpr, qreal marginFrac)
{
    m_camera.fitRect(world, viewportPx, dpr, marginFrac);
    emit cameraChanged();
}

void CanvasController::centerOnWorld(const QPointF &world, const QSizeF &viewportPx,
                                     qreal dpr)
{
    m_camera.centerOn(world, viewportPx, dpr);
    emit cameraChanged();
}

} // namespace cutpilot::render
