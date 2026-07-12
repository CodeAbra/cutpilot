#pragma once

#include <QQuickRhiItem>
#include <QPointF>

#include "cutpilot/render/CanvasController.h"
#include "cutpilot/theme/ThemeTable.h"

namespace cutpilot::render {

// The infinite GPU canvas grid layer. A QQuickRhiItem that draws the dotted grid
// through QRhi from the shared camera. It requests a repaint only when the camera or
// theme changes, so a static view issues no per-frame redraws. Pointer and keyboard
// input are owned by the node layer stacked over this item, which writes the shared
// camera; this item only reads it.
//
// All QRhi work lives in the paired renderer (CanvasRenderer); this item owns only
// the camera-read seam. The camera itself lives in the controller so every layer
// reads the same transform.
class CanvasItem : public QQuickRhiItem {
    Q_OBJECT
    Q_PROPERTY(cutpilot::render::CanvasController *controller READ controller WRITE
                   setController NOTIFY controllerChanged)

public:
    explicit CanvasItem(QQuickItem *parent = nullptr);

    QQuickRhiItemRenderer *createRenderer() override;

    CanvasController *controller() const { return m_controller; }
    void setController(CanvasController *controller);

    // Camera state, read by the renderer during synchronize.
    qreal zoom() const { return m_controller ? m_controller->zoom() : 1.0; }
    QPointF panPixels() const
    {
        return m_controller ? m_controller->panPixels() : QPointF(0, 0);
    }

    const theme::ThemeTable &themeTable() const { return m_theme; }

signals:
    void controllerChanged();

private:
    CanvasController *m_controller = nullptr;
    theme::ThemeTable m_theme{theme::Theme::Dark};
};

} // namespace cutpilot::render
