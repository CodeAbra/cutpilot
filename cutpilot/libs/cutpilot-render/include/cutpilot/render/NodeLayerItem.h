#pragma once

#include <QHash>
#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QSet>
#include <QQuickItem>
#include <QStringList>
#include <QVector>

#include "cutpilot/core/ComfyImport.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/PortRules.h"
#include "cutpilot/core/SpatialIndex.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/theme/ThemeTable.h"

QT_BEGIN_NAMESPACE
class QSGNode;
class QSGTransformNode;
class QTimer;
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
    // The active canvas tool. Cursor selects, moves, and marquees; Cut slices
    // connectors under a click or a stroke; Connect wires node to node without
    // needing a precise port grab; Place drops the armed prototype on the next
    // click and returns to Cursor.
    enum class Tool {
        Cursor,
        Cut,
        Connect,
        Place
    };
    Q_ENUM(Tool)

    explicit NodeLayerItem(QQuickItem *parent = nullptr);

    CanvasController *controller() const { return m_controller; }
    void setController(CanvasController *controller);

    core::NodeGraph &graph() { return m_graph; }

    // The theme every card and overlay draws from; switching repaints the board.
    void setTheme(theme::Theme themeId);
    theme::Theme themeId() const { return m_theme.theme(); }

    Tool tool() const { return m_tool; }
    void setTool(Tool tool);

    // Arm the Place tool with a prototype; the next canvas click drops it there.
    void armPlacement(const core::Node &prototype);

    // Undo/redo over the canvas history, mirroring the keyboard path, so the
    // chrome's history controls drive the same stack.
    void undo();
    void redo();
    bool canUndo() const { return m_commands.canUndo(); }
    bool canRedo() const { return m_commands.canRedo(); }

    // The union of every node's world rect; a null rect on an empty board.
    QRectF contentWorldBounds() const;

    // The node's media average color for the minimap block, or an invalid
    // color when the node has no decoded media.
    QColor nodeAverageColor(int nodeId) const
    {
        return m_mediaAverages.value(nodeId, QColor());
    }

    // Drop a prototype centered on a world point as one undo step; returns the
    // assigned id. The seam behind the tool pill, the command palette, and the
    // asset browser.
    int placePrototypeAt(const core::Node &prototype, const QPointF &worldCentre);

    // Drop a wired template centered on a world point as one undo step; wires
    // reference the prototype list by index. Returns the assigned ids.
    QVector<int> placeSubgraphAt(const QVector<core::Node> &prototypes,
                                 const QVector<core::Connection> &indexWires,
                                 const QPointF &worldCentre);

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

    // Undoable content edits, pushed through the command stack like any other
    // graph mutation. The chrome calls these from its prompt editor, model
    // picker, gate limit dialog, file picker, and parameter inspector.
    void setNodePrompt(int nodeId, const QString &text);
    void setNodeModel(int nodeId, const QString &modelId, const QString &modelLabel);
    void setGateLimit(int nodeId, double limitUsd);
    void setNodeMediaPath(int nodeId, const QString &mediaPath);

    // The scrub seam, mirroring how a drag moves nodes: the inspector writes
    // parameters directly for live feedback, then records the whole gesture
    // as one undoable step on release without re-applying it.
    void previewCompositeParams(int nodeId, const core::CompositeParams &params);
    void commitCompositeParams(int nodeId, const core::CompositeParams &before,
                               const core::CompositeParams &after);

    // Seed a local compositing chain — stills through a key and a transform
    // into a blend — for demos and evidence runs, no vendor required. The
    // still images are generated and written under the system temp location.
    Q_INVOKABLE void seedCompositeBoard();

    // Land a mapped ComfyUI workflow on the board as one undo step. The
    // outcome carries the per-node tier report for the chrome to present.
    core::ComfyImportOutcome importComfyWorkflow(const QJsonObject &result,
                                                 const QPointF &worldOrigin);

    // Hand the layer a node's decoded result to display as the card's media
    // body. Images stay keyed by node id, which is never reused, so a node
    // restored by undo finds its media again. Clearing drops the image and
    // returns the card to its textual body.
    void setNodeMedia(int nodeId, const QImage &image);
    void clearNodeMedia(int nodeId);

    // The node's current media image and its upload version — the preview
    // and the thumbnail compositor read their source pixels from here.
    QImage nodeMediaImage(int nodeId) const { return m_mediaImages.value(nodeId); }
    int nodeMediaVersion(int nodeId) const
    {
        return m_mediaVersions.value(nodeId, -1);
    }

    // A node's content or status changed outside a canvas gesture (a run
    // progressing, a model registry landing); refresh its card.
    void refreshNode(int nodeId);

    // A node's body region in the item's logical pixels, for the chrome to
    // place an overlay editor over it. Empty when the node is gone.
    QRectF nodeBodyScreenRect(int nodeId) const;

    // Whether the connector pulse that accompanies an in-flight generation is
    // ticking.
    bool generationPulseActive() const;

signals:
    void controllerChanged();

    // A fresh connector was dropped on empty canvas; the chrome should present the
    // palette (paletteEntryTitles) and answer with placePaletteEntry or cancelPalette.
    void paletteRequested();

    // The palette was summoned without a wire — Tab, or a double-click on empty
    // canvas — carrying the world point a picked node should land on.
    void paletteInvoked(QPointF worldPos);

    // The active tool changed, or a one-shot tool completed and fell back to
    // the cursor; the tool pill mirrors this state.
    void toolChanged();

    // Anything the minimap shows moved: graph structure, a live drag, media.
    void boardChanged();

    // The run control on a generation node was pressed.
    void runRequested(int nodeId);
    void stopRequested(int nodeId);

    // The node's prompt body wants an editor, or its model chip wants the
    // registry-driven picker; the chrome supplies both surfaces.
    void promptEditRequested(int nodeId);
    void modelPickerRequested(int nodeId);

    // A generation node was right-pressed and wants its run menu (run to
    // here, re-run ignoring cache); a cost gate wants its limit editor.
    void nodeMenuRequested(int nodeId);
    void gateLimitEditRequested(int nodeId);

    // A still or video node wants the file picker; a compositing node wants
    // the parameter inspector; a loaded video wants its transport. The
    // chrome supplies these surfaces.
    void mediaPickRequested(int nodeId);
    void compositeEditRequested(int nodeId);
    void videoTransportRequested(int nodeId);

    // The graph structure or content changed through a command or history
    // walk; run bookkeeping (orphaned states, dead jobs) should reconcile.
    void graphMutated();

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

    // Start or stop the connector pulse depending on whether any generation
    // is in flight.
    void updatePulseTimer();

    // Refresh the live wiring overlay: the in-flight curve and the target-port cue.
    void updateLive(QSGNode *liveRoot);

    // Add, remove, or refresh the screen-space overlay children (the marquee band)
    // under the container root.
    void updateOverlay(QSGNode *root, QSGTransformNode *camera);

    // The connection whose curve passes within the slice reach of the world
    // point, or -1. The reach floor keeps connectors sliceable when zoomed out.
    int connectionNear(const QPointF &world) const;

    // Cut-tool slicing: remove every connection the stroke touches, each as
    // its own undo step, so an accidental slice is one undo away.
    void sliceAt(const QPointF &world);

    // Connect-tool wiring from a node body: anchor the drag on the node's
    // first output (or, failing that, first input) port.
    bool beginConnectFromNode(int nodeId, const QPointF &world);

    // The target node's best port compatible with the live wire's anchor —
    // a direct match beats a conversion — or -1.
    int bestCompatiblePort(const core::Node &node) const;

    CanvasController *m_controller = nullptr;
    core::NodeGraph m_graph;
    core::SpatialIndex m_index;
    core::CommandStack m_commands;
    theme::ThemeTable m_theme{theme::Theme::Dark};

    Tool m_tool = Tool::Cursor;
    core::Node m_placePrototype;
    bool m_slicing = false;

    // Decoded result images by node id, with a version that invalidates the
    // uploaded texture when a new result replaces an old one, and each image's
    // average color for the minimap.
    QHash<int, QImage> m_mediaImages;
    QHash<int, int> m_mediaVersions;
    QHash<int, QColor> m_mediaAverages;

    // Connectors feeding an in-flight generation shimmer; the timer advances
    // the phase only while at least one job runs.
    QTimer *m_pulseTimer = nullptr;
    int m_pulseFrame = 0;
    int m_lastPulseFrame = -1;

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

    // The pan gesture remembers the button that started it, so releasing a
    // different button (a concurrent left-button gesture) never ends the pan.
    bool m_panning = false;
    Qt::MouseButton m_panButton = Qt::NoButton;
    bool m_spaceHeld = false;
    QPointF m_lastPanPosLogical;
};

} // namespace cutpilot::render
