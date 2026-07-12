#pragma once

#include <QPointF>
#include <QQuickItem>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/theme/ThemeTable.h"

QT_BEGIN_NAMESPACE
class QSGTransformNode;
QT_END_NAMESPACE

namespace cutpilot::render {

// The node layer: a scene-graph QQuickItem stacked over the grid that draws the node
// model as vertex-colored geometry and handles node selection and drag. Each node is
// one scene-graph geometry node built in world space under a single transform node
// that carries the shared camera, so pan and zoom are a matrix change and never
// re-triangulate any node. Geometry rebuilds only when the model or the detail tier
// changes, so a static board — and every pan or in-tier zoom — issues no rebuild.
//
// Hit-testing runs in world space against the model, not via Qt item picking, so it
// is ready to back onto a spatial index as the board grows.
class NodeLayerItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(cutpilot::render::CanvasController *controller READ controller WRITE
                   setController NOTIFY controllerChanged)

public:
    explicit NodeLayerItem(QQuickItem *parent = nullptr);

    CanvasController *controller() const { return m_controller; }
    void setController(CanvasController *controller);

    core::NodeGraph &graph() { return m_graph; }

    // Seed the starting board with a single content-first prompt node.
    Q_INVOKABLE void seedStarterNode();

signals:
    void controllerChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    qreal devicePixelRatio() const;
    QPointF worldFromLocal(const QPointF &localLogical) const;
    QPointF panLogical() const;

    // Reconcile one child geometry node per model node under the camera transform,
    // reusing existing children and their buffers where the vertex counts still fit.
    void rebuildNodes(QSGTransformNode *root, bool detailed);

    CanvasController *m_controller = nullptr;
    core::NodeGraph m_graph;
    theme::ThemeTable m_theme{theme::Theme::Dark};

    // Rebuild node geometry only when the model or the detail tier changes; a plain
    // camera move just re-sets the transform matrix.
    bool m_geometryDirty = true;
    bool m_lastDetailed = true;

    bool m_dragging = false;
    int m_dragNodeId = -1;
    QPointF m_dragGrabWorldOffset;

    bool m_panning = false;
    bool m_spaceHeld = false;
    QPointF m_lastPanPosLogical;
};

} // namespace cutpilot::render
