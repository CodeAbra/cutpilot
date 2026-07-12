#include "cutpilot/render/NodeLayerItem.h"
#include "NodeGeometryBuilder.h"

#include "cutpilot/core/AlignmentGuides.h"
#include "cutpilot/core/command/AddNodeCommand.h"
#include "cutpilot/core/command/DeleteNodesCommand.h"
#include "cutpilot/core/command/MoveNodesCommand.h"

#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <QWheelEvent>
#include <QtMath>

#include <memory>

namespace cutpilot::render {

namespace {

// The marquee outline in the item's logical pixels: a constant hairline at any zoom.
constexpr qreal kMarqueeOutlineWidth = 1.0;

// The alignment-guide hairline width and the pixel radius within which a dragged edge
// snaps a guide into view, both in the item's logical pixels.
constexpr qreal kGuideWidth = 1.0;
constexpr qreal kGuidePixelThreshold = 6.0;

// Copy a built mesh into a geometry node, reusing the existing buffers unless the
// vertex or index counts changed.
void uploadMesh(QSGGeometryNode *node, const NodeGeometryBuilder::Mesh &mesh)
{
    QSGGeometry *geometry = node->geometry();
    if (!geometry || geometry->vertexCount() != mesh.vertices.size()
        || geometry->indexCount() != mesh.indices.size()) {
        geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                   mesh.vertices.size(), mesh.indices.size());
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
    }

    auto *vd = geometry->vertexDataAsColoredPoint2D();
    for (int i = 0; i < mesh.vertices.size(); ++i) {
        const auto &v = mesh.vertices[i];
        vd[i].set(v.x, v.y, v.r, v.g, v.b, v.a);
    }
    quint16 *id = geometry->indexDataAsUShort();
    for (int i = 0; i < mesh.indices.size(); ++i)
        id[i] = mesh.indices[i];

    node->markDirty(QSGNode::DirtyGeometry);
}

QSGGeometryNode *makeGeometryNode()
{
    auto *node = new QSGGeometryNode;
    auto *material = new QSGVertexColorMaterial;
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    node->setFlag(QSGNode::OwnsGeometry, true);
    return node;
}

} // namespace

NodeLayerItem::NodeLayerItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setFocus(true);
}

void NodeLayerItem::setController(CanvasController *controller)
{
    if (m_controller == controller)
        return;

    if (m_controller)
        m_controller->disconnect(this);

    m_controller = controller;

    if (m_controller) {
        // Repaint the node layer whenever the shared camera changes so nodes stay
        // pinned to the world as the canvas pans and zooms.
        connect(m_controller, &CanvasController::cameraChanged, this,
                [this] { update(); });
    }

    emit controllerChanged();
    update();
}

core::Node NodeLayerItem::defaultNode(const QPointF &worldCentre) const
{
    core::Node node;
    node.title = QStringLiteral("Node");
    const QSizeF size(280.0, 200.0);
    node.worldSize = size;
    node.worldPos =
        worldCentre - QPointF(size.width() / 2.0, size.height() / 2.0);
    node.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.42 },
        { QStringLiteral("text"), core::PortType::Text, true, 0.66 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    return node;
}

void NodeLayerItem::seedStarterNode()
{
    core::Node node;
    node.title = QStringLiteral("Prompt");
    node.worldPos = QPointF(160.0, 120.0);
    node.worldSize = QSizeF(280.0, 200.0);
    node.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.42 },
        { QStringLiteral("text"), core::PortType::Text, true, 0.66 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    m_graph.addNode(node);
    syncSpatialIndex();
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::seedStressBoard(int count)
{
    constexpr int kMaxStressNodes = 5000;
    const int total = qBound(0, count, kMaxStressNodes);
    if (total == 0)
        return;

    // A wide grid: spacing well beyond a node's size so any viewport holds only a
    // fraction of the board.
    const int columns = qMax(1, qCeil(qSqrt(qreal(total))));
    constexpr qreal spacingX = 520.0;
    constexpr qreal spacingY = 420.0;
    const QPointF centreOffset(140.0, 100.0);

    for (int i = 0; i < total; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        const QPointF topLeft(column * spacingX, row * spacingY);
        core::Node node = defaultNode(topLeft + centreOffset);
        node.title = QStringLiteral("Node %1").arg(i + 1);
        m_graph.addNode(node);
    }

    syncSpatialIndex();
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::addNodeAtCursor(const QPointF &worldPoint)
{
    m_commands.push(std::make_unique<core::AddNodeCommand>(defaultNode(worldPoint)),
                    m_graph);
    syncSpatialIndex();
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::syncSpatialIndex()
{
    m_index.rebuild(m_graph.nodes());
}

int NodeLayerItem::pickTopMost(const QPointF &world) const
{
    // The point query narrows candidates through the index; the top-most is the one
    // latest in the model's z-order (list order).
    const QVector<int> candidates = m_index.queryPoint(world);
    int bestId = -1;
    int bestIndex = -1;
    for (int id : candidates) {
        const int index = m_graph.indexOfId(id);
        if (index > bestIndex) {
            bestIndex = index;
            bestId = id;
        }
    }
    return bestId;
}

QVector<int> NodeLayerItem::visibleForViewport() const
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const QSizeF viewport(width(), height());
    return core::visibleIds(m_index, zoom, panLogical(), viewport);
}

qreal NodeLayerItem::devicePixelRatio() const
{
    return window() ? window()->effectiveDevicePixelRatio() : 1.0;
}

QPointF NodeLayerItem::panLogical() const
{
    if (!m_controller)
        return QPointF(0, 0);
    return m_controller->panPixels() / devicePixelRatio();
}

QPointF NodeLayerItem::worldFromLocal(const QPointF &localLogical) const
{
    if (!m_controller)
        return localLogical;
    // The item's local coordinates are logical pixels; the camera works in physical
    // pixels, so scale up by the device pixel ratio before unprojecting.
    const qreal dpr = devicePixelRatio();
    return m_controller->worldFromScreen(localLogical * dpr, dpr);
}

QPointF NodeLayerItem::localFromWorld(const QPointF &world) const
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    return world * zoom + panLogical();
}

void NodeLayerItem::updateDragGuides()
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;

    // The moving reference is the grabbed node for a single drag, or the selection's
    // bounding box for a multi-node drag.
    const QSet<int> moving(m_dragIds.cbegin(), m_dragIds.cend());
    QRectF movingRect;
    for (int id : m_dragIds) {
        if (const core::Node *n = m_graph.nodeById(id))
            movingRect = movingRect.isNull() ? n->worldRect()
                                             : movingRect.united(n->worldRect());
    }

    // Guides key only off nodes the user can see: the culled visible set minus the
    // moving nodes. Snapping to off-screen geometry would draw a guide "for no reason",
    // and scoping to the viewport also bounds the per-move cost.
    QVector<QRectF> neighbours;
    for (int id : visibleForViewport()) {
        if (moving.contains(id))
            continue;
        if (const core::Node *n = m_graph.nodeById(id))
            neighbours.push_back(n->worldRect());
    }

    m_guideLinesLogical.clear();
    if (!movingRect.isNull()) {
        const qreal worldThreshold = kGuidePixelThreshold / (zoom != 0.0 ? zoom : 1.0);
        const auto guides = core::computeAlignmentGuides(movingRect, neighbours, worldThreshold);
        for (const core::AlignmentGuide &g : guides) {
            if (g.axis == core::GuideAxis::Vertical) {
                const QPointF a = localFromWorld(QPointF(g.coordinate, g.spanStart));
                const QPointF b = localFromWorld(QPointF(g.coordinate, g.spanEnd));
                m_guideLinesLogical.push_back(QLineF(a, b));
            } else {
                const QPointF a = localFromWorld(QPointF(g.spanStart, g.coordinate));
                const QPointF b = localFromWorld(QPointF(g.spanEnd, g.coordinate));
                m_guideLinesLogical.push_back(QLineF(a, b));
            }
        }
    }

    m_guidesActive = !m_guideLinesLogical.isEmpty();
    m_overlayDirty = true;
}

QSGNode *NodeLayerItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    QSGNode *root = oldNode;
    QSGTransformNode *camera = nullptr;
    if (!root) {
        // A plain container: a camera transform child in world space plus, on demand,
        // screen-space overlay children drawn in the item's logical pixels.
        root = new QSGNode;
        camera = new QSGTransformNode;
        root->appendChildNode(camera);
        m_geometryDirty = true;
        m_overlayDirty = true;
    } else {
        camera = static_cast<QSGTransformNode *>(root->firstChild());
    }

    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const QPointF pan = panLogical();

    // The camera transform maps world coordinates to the item's logical pixels, so a
    // pan or zoom is a matrix change here and never re-triangulates any node.
    QMatrix4x4 matrix;
    matrix.translate(float(pan.x()), float(pan.y()));
    matrix.scale(float(zoom), float(zoom));
    camera->setMatrix(matrix);

    // Cull to the nodes overlapping the viewport. The subtree is rebuilt when the model
    // changed, the detail tier flipped, or the visible membership changed; a pan or
    // in-tier zoom whose visible set is unchanged re-sets only the transform matrix.
    const QVector<int> visibleList = visibleForViewport();
    const QSet<int> visible(visibleList.cbegin(), visibleList.cend());
    const bool membershipChanged = visible != m_lastVisibleSet;

    const bool detailed = zoom >= NodeGeometryBuilder::kDetailZoom;
    if (m_geometryDirty || detailed != m_lastDetailed || membershipChanged) {
        rebuildNodes(camera, detailed, visible);
        m_lastDetailed = detailed;
        m_lastVisibleSet = visible;
        m_geometryDirty = false;
    }

    updateOverlay(root, camera);
    return root;
}

void NodeLayerItem::rebuildNodes(QSGTransformNode *camera, bool detailed,
                                 const QSet<int> &visible)
{
    NodeGeometryBuilder builder;

    QSGNode *child = camera->firstChild();
    for (const core::Node &node : m_graph.nodes()) {
        if (!visible.contains(node.id))
            continue; // off-screen nodes are not drawn

        const NodeGeometryBuilder::Mesh mesh =
            builder.buildNode(node, m_theme, detailed);

        auto *geometryNode = static_cast<QSGGeometryNode *>(child);
        if (!geometryNode) {
            geometryNode = makeGeometryNode();
            camera->appendChildNode(geometryNode);
        }

        uploadMesh(geometryNode, mesh);
        child = geometryNode->nextSibling();
    }

    // Drop any geometry nodes left over from a larger previous board.
    while (child) {
        QSGNode *next = child->nextSibling();
        camera->removeChildNode(child);
        delete child;
        child = next;
    }
}

void NodeLayerItem::updateOverlay(QSGNode *root, QSGTransformNode *camera)
{
    // A single screen-space overlay child sits beside the camera transform, so its
    // vertices draw in the item's logical pixels. The marquee band and the alignment
    // guides are mutually exclusive gestures, so one geometry node carries whichever is
    // active.
    auto *overlay = static_cast<QSGGeometryNode *>(camera->nextSibling());
    const bool active = m_marqueeActive || m_guidesActive;

    if (active) {
        if (!overlay) {
            overlay = makeGeometryNode();
            root->appendChildNode(overlay);
            m_overlayDirty = true;
        }
        if (m_overlayDirty) {
            NodeGeometryBuilder builder;
            if (m_marqueeActive) {
                const QRectF rect =
                    QRectF(m_marqueeStartLogical, m_marqueeCurrentLogical).normalized();
                uploadMesh(overlay,
                           builder.buildScreenRect(rect, m_theme.selectionFill(),
                                                   m_theme.selection(),
                                                   kMarqueeOutlineWidth));
            } else {
                uploadMesh(overlay, builder.buildScreenLines(m_guideLinesLogical,
                                                             m_theme.emphasis(),
                                                             kGuideWidth));
            }
        }
    } else if (overlay) {
        root->removeChildNode(overlay);
        delete overlay;
    }

    m_overlayDirty = false;
}

void NodeLayerItem::mousePressEvent(QMouseEvent *event)
{
    if (!m_controller) {
        event->ignore();
        return;
    }
    forceActiveFocus();

    // Middle-button, or Space + left-button, pans the canvas; handled on the top layer
    // so a node hit and a pan never fight over the same press.
    const bool panButton = event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && m_spaceHeld);
    if (panButton) {
        m_panning = true;
        m_lastPanPosLogical = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPointF world = worldFromLocal(event->position());
    const int hitId = pickTopMost(world);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);

    if (hitId != -1) {
        if (shift) {
            m_graph.toggleSelected(hitId);
        } else if (!m_graph.nodeById(hitId)->selected) {
            // A plain click on an unselected node makes it the only selection; a plain
            // click on an already-selected node keeps the whole selection to drag it.
            m_graph.selectOnly(hitId);
        }

        if (m_graph.nodeById(hitId) && m_graph.nodeById(hitId)->selected) {
            const QVector<int> ids = m_graph.selectedIds();
            // Raise-on-touch is a deliberate, non-undoable z reorder: it is not routed
            // through the command stack, so add/move/delete undo never restores a prior z.
            m_graph.raiseToTop(ids);
            m_dragging = true;
            m_dragIds = m_graph.selectedIds();
            m_dragPressWorld = world;
            m_dragLastWorld = world;
            setCursor(Qt::ClosedHandCursor);
        }
        m_geometryDirty = true;
        update();
    } else {
        // Empty canvas: a plain press clears the selection and starts a marquee band;
        // Shift keeps the current selection so the band adds to it.
        if (!shift)
            m_graph.clearSelection();
        m_marqueeActive = true;
        m_marqueeAdditive = shift;
        m_marqueeStartLogical = event->position();
        m_marqueeCurrentLogical = event->position();
        m_overlayDirty = true;
        m_geometryDirty = true;
        update();
    }
    event->accept();
}

void NodeLayerItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning && m_controller) {
        const QPointF deltaLogical = event->position() - m_lastPanPosLogical;
        m_lastPanPosLogical = event->position();
        m_controller->panByPixels(deltaLogical * devicePixelRatio()); // 1:1, no smoothing
        event->accept();
        return;
    }

    if (m_marqueeActive) {
        m_marqueeCurrentLogical = event->position();
        m_overlayDirty = true;
        update();
        event->accept();
        return;
    }

    if (m_dragging) {
        const QPointF world = worldFromLocal(event->position());
        const QPointF delta = world - m_dragLastWorld;
        m_dragLastWorld = world;
        m_graph.moveNodesBy(m_dragIds, delta); // live 1:1 feedback
        // Only the dragged nodes moved, so update just their index entries rather than
        // rebuilding the whole index on every move.
        for (int id : m_dragIds) {
            if (const core::Node *n = m_graph.nodeById(id))
                m_index.update(id, n->worldRect());
        }
        updateDragGuides();
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    event->ignore();
}

void NodeLayerItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_panning = false;
        if (!m_spaceHeld)
            unsetCursor();
        event->accept();
        return;
    }

    if (m_marqueeActive) {
        const QPointF worldStart = worldFromLocal(m_marqueeStartLogical);
        const QPointF worldEnd = worldFromLocal(m_marqueeCurrentLogical);
        m_graph.selectInRect(QRectF(worldStart, worldEnd).normalized(), m_marqueeAdditive);
        m_marqueeActive = false;
        m_overlayDirty = true;
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    if (m_dragging) {
        // Coalesce the whole drag into one net-delta move: the nodes already moved live
        // by this delta, so record (do not re-apply) it as a single undo step.
        const QPointF net = m_dragLastWorld - m_dragPressWorld;
        if (!net.isNull())
            m_commands.record(std::make_unique<core::MoveNodesCommand>(m_dragIds, net));
        m_dragging = false;
        m_dragIds.clear();
        // The guides are a drag hint; clear them when the gesture ends.
        m_guidesActive = false;
        m_guideLinesLogical.clear();
        m_overlayDirty = true;
        unsetCursor();
        update();
        event->accept();
        return;
    }

    event->ignore();
}

void NodeLayerItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_controller || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPointF world = worldFromLocal(event->position());
    if (pickTopMost(world) == -1) {
        addNodeAtCursor(world); // empty canvas: add a node at the cursor
        event->accept();
        return;
    }
    event->ignore(); // on a node, a double-click does not add
}

void NodeLayerItem::wheelEvent(QWheelEvent *event)
{
    if (!m_controller) {
        event->ignore();
        return;
    }

    const qreal dpr = devicePixelRatio();
    const QPointF anchorPx = event->position() * dpr;

    // Pinch or Ctrl+wheel zooms toward the cursor; a plain two-finger scroll pans.
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const qreal steps = event->angleDelta().y() / 120.0;
        const qreal factor = qPow(1.15, steps);
        m_controller->zoomAbout(anchorPx, factor, dpr);
        event->accept();
        return;
    }

    QPointF deltaLogical;
    if (!event->pixelDelta().isNull())
        deltaLogical = event->pixelDelta();
    else
        deltaLogical = event->angleDelta() / 4.0;

    m_controller->panByPixels(deltaLogical * dpr);
    event->accept();
}

void NodeLayerItem::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) {
        m_spaceHeld = true;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }

    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);

    if (control && event->key() == Qt::Key_0) {
        if (m_controller)
            m_controller->reset();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        const QVector<int> ids = m_graph.selectedIds();
        if (!ids.isEmpty()) {
            m_commands.push(std::make_unique<core::DeleteNodesCommand>(ids), m_graph);
            syncSpatialIndex();
            m_geometryDirty = true;
            update();
        }
        event->accept();
        return;
    }

    if (control && event->key() == Qt::Key_Z) {
        if (shift)
            m_commands.redo(m_graph);
        else
            m_commands.undo(m_graph);
        syncSpatialIndex();
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    if (control && event->key() == Qt::Key_A) {
        for (const core::Node &n : m_graph.nodes())
            m_graph.setSelected(n.id, true);
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        m_graph.clearSelection();
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    event->ignore();
}

void NodeLayerItem::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        if (!m_panning && !m_dragging)
            unsetCursor();
        event->accept();
        return;
    }
    event->ignore();
}

} // namespace cutpilot::render
