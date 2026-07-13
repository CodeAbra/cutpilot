#pragma once

#include <QQuickItem>

#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/MinimapProjection.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

namespace cutpilot::render {

// The board overview: every node as an average-color block, connectors as
// faint lines, and the camera's viewport as a rectangle, drawn as scene-graph
// geometry over the same node model and camera the main canvas uses. Clicking
// or dragging inside it recenters the camera on the corresponding world point.
// It repaints on board or camera changes only, never per frame.
class MinimapItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(cutpilot::render::NodeLayerItem *layer READ layer WRITE setLayer
                   NOTIFY layerChanged)
    Q_PROPERTY(cutpilot::render::CanvasController *controller READ controller
                   WRITE setController NOTIFY controllerChanged)

public:
    explicit MinimapItem(QQuickItem *parent = nullptr);

    NodeLayerItem *layer() const { return m_layer; }
    void setLayer(NodeLayerItem *layer);

    CanvasController *controller() const { return m_controller; }
    void setController(CanvasController *controller);

    void setTheme(theme::Theme themeId);

    // The current world-to-minimap mapping: the board's bounds united with the
    // camera's viewport, framed inside this item. Pure and deterministic, so
    // painting, input, and tests share one mapping.
    MinimapProjection projection() const;

    // The painted footprint of one node's block, in this item's logical pixels.
    QRectF blockRectFor(int nodeId) const;

    // The painted viewport rectangle, in this item's logical pixels.
    QRectF viewportMiniRect() const;

    // The world point under an item-local position.
    QPointF worldAtItemPos(const QPointF &itemPos) const;

signals:
    void layerChanged();
    void controllerChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    qreal devicePixelRatio() const;

    // The camera's viewport in world coordinates, derived from the canvas
    // layer's size and the shared camera.
    QRectF viewportWorldRect() const;

    void jumpTo(const QPointF &itemPos);

    NodeLayerItem *m_layer = nullptr;
    CanvasController *m_controller = nullptr;
    theme::ThemeTable m_theme{theme::Theme::Dark};
    bool m_navigating = false;
    MinimapProjection m_dragProjection;
};

} // namespace cutpilot::render
