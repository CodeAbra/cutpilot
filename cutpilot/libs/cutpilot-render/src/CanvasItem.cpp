#include "cutpilot/render/CanvasItem.h"
#include "CanvasRenderer.h"

namespace cutpilot::render {

CanvasItem::CanvasItem(QQuickItem *parent)
    : QQuickRhiItem(parent)
{
}

QQuickRhiItemRenderer *CanvasItem::createRenderer()
{
    return new CanvasRenderer;
}

void CanvasItem::setController(CanvasController *controller)
{
    if (m_controller == controller)
        return;

    if (m_controller)
        m_controller->disconnect(this);

    m_controller = controller;

    if (m_controller) {
        // Repaint the grid whenever any layer changes the shared camera.
        connect(m_controller, &CanvasController::cameraChanged, this,
                [this] { update(); });
    }

    emit controllerChanged();
    update();
}

} // namespace cutpilot::render
