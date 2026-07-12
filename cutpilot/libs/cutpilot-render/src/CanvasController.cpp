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

} // namespace cutpilot::render
