#pragma once

#include <QLineF>
#include <QPointF>
#include <QQuickItem>
#include <QSet>
#include <QVector>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/SpatialIndex.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/theme/ThemeTable.h"

QT_BEGIN_NAMESPACE
class QSGNode;
class QSGTransformNode;
QT_END_NAMESPACE

namespace cutpilot::render {

// The node layer: a scene-graph QQuickItem stacked over the grid that draws the node
// model and handles selection, marquee, drag, add, delete, and undo/redo. The
// scene-graph root is a plain container: a camera transform child holds one geometry
// node per node in world space, so pan and zoom are a matrix change and never
// re-triangulate a node; a screen-space marquee child is drawn in the item's logical
// pixels, so its outline holds a constant width at any zoom.
//
// Every model mutation runs here on the GUI thread through the command stack, so a
// gesture is undoable and updatePaintNode only ever reads the model. Hit-testing runs
// in world space against the model. Touching a node raises it to the top of the
// z-order.
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

    // Create a default content-first node centred on a world point and add it through
    // an undoable command. The reusable creation seam a double-click, and later a
    // palette or tool pill, calls.
    Q_INVOKABLE void addNodeAtCursor(const QPointF &worldPoint);

signals:
    void controllerChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    qreal devicePixelRatio() const;
    QPointF worldFromLocal(const QPointF &localLogical) const;
    QPointF localFromWorld(const QPointF &world) const;
    QPointF panLogical() const;

    core::Node defaultNode(const QPointF &worldCentre) const;

    // Recompute the alignment guides for the current drag and store them as screen-space
    // lines for the overlay. Clears them when the drag aligns with nothing.
    void updateDragGuides();

    // Rebuild the spatial index from the model. Called after any add, move, delete, or
    // restore so culling and picking stay in step with the model.
    void syncSpatialIndex();

    // The top-most node containing the world point through the spatial index, or -1.
    int pickTopMost(const QPointF &world) const;

    // The ids overlapping the current viewport under the camera, through the index.
    QVector<int> visibleForViewport() const;

    // Reconcile one child geometry node per visible model node under the camera
    // transform, reusing existing children and their buffers where the vertex counts
    // still fit. Only nodes in the visible set are meshed.
    void rebuildNodes(QSGTransformNode *camera, bool detailed, const QSet<int> &visible);

    // Add, remove, or refresh the screen-space overlay children (the marquee band)
    // under the container root.
    void updateOverlay(QSGNode *root, QSGTransformNode *camera);

    CanvasController *m_controller = nullptr;
    core::NodeGraph m_graph;
    core::SpatialIndex m_index;
    core::CommandStack m_commands;
    theme::ThemeTable m_theme{theme::Theme::Dark};

    // Rebuild node geometry only when the model or the detail tier changes; a plain
    // camera move just re-sets the transform matrix. Overlay dirtiness is tracked
    // apart so a marquee update never rebuilds the node geometry.
    bool m_geometryDirty = true;
    bool m_lastDetailed = true;
    bool m_overlayDirty = false;

    // The visible id set the child subtree currently holds; the subtree is rebuilt only
    // when this membership changes, so a pan that keeps the same nodes visible re-sets
    // only the transform matrix.
    QSet<int> m_lastVisibleSet;

    // A selection drag: the grabbed set moves live for 1:1 feedback and coalesces into
    // one net-delta move command on release.
    bool m_dragging = false;
    QVector<int> m_dragIds;
    QPointF m_dragPressWorld;
    QPointF m_dragLastWorld;

    // A marquee band drawn in the item's logical pixels; additive when Shift is held.
    bool m_marqueeActive = false;
    bool m_marqueeAdditive = false;
    QPointF m_marqueeStartLogical;
    QPointF m_marqueeCurrentLogical;

    // Alignment guides for the active drag, as screen-space lines in logical pixels.
    bool m_guidesActive = false;
    QVector<QLineF> m_guideLinesLogical;

    bool m_panning = false;
    bool m_spaceHeld = false;
    QPointF m_lastPanPosLogical;
};

} // namespace cutpilot::render
