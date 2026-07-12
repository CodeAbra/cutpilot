#pragma once

#include <QLineF>
#include <QPointF>
#include <QSet>
#include <QQuickItem>
#include <QStringList>
#include <QVector>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/PortRules.h"
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
// model and handles selection, marquee, drag, add, delete, wiring, and undo/redo.
// The scene-graph root is a plain container. A camera transform child holds three
// world-space groups drawn back to front — the batched connector chunks, one
// geometry node per node, and the live wiring overlay — so pan and zoom are a
// matrix change and never re-triangulate anything. A screen-space marquee child is
// drawn in the item's logical pixels, so its outline holds a constant width at any
// zoom.
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

    // Seed the starting board: a prompt node feeding a generate node, so the first
    // canvas already shows a wired pair.
    Q_INVOKABLE void seedStarterNode();

    // Scatter a board of content-first nodes wide enough that any viewport holds only a
    // fraction, chained output-to-input so connectors share the load, for the
    // frame-budget check at scale. The count is capped.
    Q_INVOKABLE void seedStressBoard(int count);

    // Create a default content-first node centred on a world point and add it through
    // an undoable command. The reusable creation seam a double-click, and later a
    // palette or tool pill, calls.
    Q_INVOKABLE void addNodeAtCursor(const QPointF &worldPoint);

    // The palette raised by dropping a fresh connector on empty canvas. The titles
    // list only node types with a port compatible with the dragged type; placing an
    // entry adds that node at the drop point and wires it in one undo step.
    Q_INVOKABLE QStringList paletteEntryTitles() const;
    Q_INVOKABLE void placePaletteEntry(int index);
    Q_INVOKABLE void cancelPalette();

signals:
    void controllerChanged();

    // A fresh connector was dropped on empty canvas; the chrome should present the
    // palette (paletteEntryTitles) and answer with placePaletteEntry or cancelPalette.
    void paletteRequested();

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
    // A port under the cursor, or {-1, -1}.
    struct PortHit {
        int nodeId = -1;
        int portIndex = -1;
    };

    // One offered palette row: the node to place and the port that auto-connects.
    struct PaletteOffer {
        core::Node node;
        int portIndex = 0;
    };

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

    // The top-most port within reach of the world point, through the spatial index.
    // The reach floor is a comfortable on-screen hit target, so ports stay grabbable
    // when zoomed out.
    PortHit pickPort(const QPointF &world) const;

    // The ids overlapping the current viewport under the camera, through the index.
    QVector<int> visibleForViewport() const;

    // The connector world width for a zoom: a constant world weight, doubled in
    // discrete steps once its on-screen width would fall under a floor, so
    // connectors never vanish zoomed out yet a smooth zoom never re-tessellates.
    static qreal connectorWorldWidth(qreal zoom);

    // Start, update, and finish the wiring gesture. endConnectDrag clears the
    // gesture state; finish decides between commit, refusal, disconnect, and the
    // palette before ending.
    void beginConnectDrag(const PortHit &hit, const QPointF &world);
    void updateConnectTarget(const QPointF &world);
    void finishConnectDrag(const QPointF &world);
    void endConnectDrag();

    // The palette rows whose prototype carries a port compatible with the dragged
    // type, each paired with its best-matching port (a direct match beats a
    // conversion).
    QVector<PaletteOffer> paletteOffersFor(core::PortType type,
                                           bool anchorIsOutput) const;

    // Reconcile one child geometry node per visible model node under the given
    // container, reusing existing children and their buffers where the vertex counts
    // still fit. Only nodes in the visible set are meshed.
    void rebuildNodes(QSGNode *nodeRoot, bool detailed, const QSet<int> &visible);

    // Rebuild the batched connector chunks for the connections whose curve bounds
    // touch the viewport. Returns the visible connection ids so membership changes
    // can be detected cheaply.
    QVector<int> visibleConnections() const;
    void rebuildConnectors(QSGNode *connectorRoot, const QVector<int> &ids);

    // Refresh the live wiring overlay: the in-flight curve and the target-port cue.
    void updateLive(QSGNode *liveRoot);

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

    // Connector chunks are rebuilt when the model changes, the visible connection
    // membership changes, or the width step flips — never on a plain camera move.
    QVector<int> m_lastVisibleConnections;
    qreal m_lastConnectorWidth = 0.0;

    // The wiring gesture: the anchor is the fixed end (an output for a fresh drag
    // from an output or a grabbed occupied input; otherwise the input itself), the
    // free end tracks the cursor, and the hover target carries its match verdict.
    // A grabbed edge is hidden from the batch while re-routed.
    bool m_connectActive = false;
    int m_anchorNodeId = -1;
    int m_anchorPortIndex = -1;
    bool m_anchorIsOutput = false;
    core::PortType m_anchorType = core::PortType::Any;
    int m_detachedConnectionId = -1;
    QPointF m_connectCursorWorld;
    int m_targetNodeId = -1;
    int m_targetPortIndex = -1;
    core::PortMatch m_targetMatch = core::PortMatch::Incompatible;
    bool m_liveDirty = false;

    // A palette request waiting for the chrome's answer, with its filtered offers.
    bool m_palettePending = false;
    QPointF m_paletteWorldPos;
    int m_paletteAnchorNodeId = -1;
    int m_paletteAnchorPortIndex = -1;
    bool m_paletteAnchorIsOutput = false;
    QVector<PaletteOffer> m_paletteOffers;

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
