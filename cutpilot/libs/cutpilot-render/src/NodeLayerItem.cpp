#include "cutpilot/render/NodeLayerItem.h"
#include "NodeGeometryBuilder.h"

#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <QWheelEvent>
#include <QtMath>

namespace cutpilot::render {

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
    m_geometryDirty = true;
    update();
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

QSGNode *NodeLayerItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = static_cast<QSGTransformNode *>(oldNode);
    if (!root) {
        root = new QSGTransformNode;
        m_geometryDirty = true;
    }

    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const QPointF pan = panLogical();

    // The camera transform maps world coordinates to the item's logical pixels, so a
    // pan or zoom is a matrix change here and never re-triangulates any node.
    QMatrix4x4 matrix;
    matrix.translate(float(pan.x()), float(pan.y()));
    matrix.scale(float(zoom), float(zoom));
    root->setMatrix(matrix);

    const bool detailed = zoom >= NodeGeometryBuilder::kDetailZoom;
    if (m_geometryDirty || detailed != m_lastDetailed) {
        rebuildNodes(root, detailed);
        m_lastDetailed = detailed;
        m_geometryDirty = false;
    }

    return root;
}

void NodeLayerItem::rebuildNodes(QSGTransformNode *root, bool detailed)
{
    NodeGeometryBuilder builder;

    QSGNode *child = root->firstChild();
    for (const core::Node &node : m_graph.nodes()) {
        const NodeGeometryBuilder::Mesh mesh =
            builder.buildNode(node, m_theme, detailed);

        auto *geometryNode = static_cast<QSGGeometryNode *>(child);
        if (!geometryNode) {
            geometryNode = new QSGGeometryNode;
            auto *material = new QSGVertexColorMaterial;
            geometryNode->setMaterial(material);
            geometryNode->setFlag(QSGNode::OwnsMaterial, true);
            geometryNode->setFlag(QSGNode::OwnsGeometry, true);
            root->appendChildNode(geometryNode);
        }

        // Reuse the existing buffers unless the vertex or index counts changed;
        // OwnsGeometry frees the previous geometry when a new one is set.
        QSGGeometry *geometry = geometryNode->geometry();
        if (!geometry || geometry->vertexCount() != mesh.vertices.size()
            || geometry->indexCount() != mesh.indices.size()) {
            geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                       mesh.vertices.size(), mesh.indices.size());
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometryNode->setGeometry(geometry);
        }

        auto *vd = geometry->vertexDataAsColoredPoint2D();
        for (int i = 0; i < mesh.vertices.size(); ++i) {
            const auto &v = mesh.vertices[i];
            vd[i].set(v.x, v.y, v.r, v.g, v.b, v.a);
        }
        quint16 *id = geometry->indexDataAsUShort();
        for (int i = 0; i < mesh.indices.size(); ++i)
            id[i] = mesh.indices[i];

        geometryNode->markDirty(QSGNode::DirtyGeometry);

        child = geometryNode->nextSibling();
    }

    // Drop any geometry nodes left over from a larger previous board.
    while (child) {
        QSGNode *next = child->nextSibling();
        root->removeChildNode(child);
        delete child;
        child = next;
    }
}

void NodeLayerItem::mousePressEvent(QMouseEvent *event)
{
    if (!m_controller) {
        event->ignore();
        return;
    }
    forceActiveFocus();

    // Middle-button, or Space + left-button, pans the canvas; handled on the top
    // layer so a node hit and a pan never fight over the same press.
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
    const int hitId = m_graph.hitTest(world);
    const bool changed = m_graph.selectOnly(hitId);

    if (hitId != -1) {
        m_dragging = true;
        m_dragNodeId = hitId;
        const core::Node *n = m_graph.nodeById(hitId);
        m_dragGrabWorldOffset = world - n->worldPos;
        setCursor(Qt::ClosedHandCursor);
    } else {
        // A plain left-press on empty canvas clears the selection and starts a pan,
        // so dragging the empty surface moves the camera 1:1.
        m_panning = true;
        m_lastPanPosLogical = event->position();
        setCursor(Qt::ClosedHandCursor);
    }

    if (changed) {
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

    if (m_dragging && m_dragNodeId != -1) {
        const QPointF world = worldFromLocal(event->position());
        m_graph.moveNodeTo(m_dragNodeId, world - m_dragGrabWorldOffset);
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
    if (m_dragging) {
        m_dragging = false;
        m_dragNodeId = -1;
        unsetCursor();
        event->accept();
        return;
    }
    event->ignore();
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
    if (event->key() == Qt::Key_0
        && event->modifiers().testFlag(Qt::ControlModifier)) {
        if (m_controller)
            m_controller->reset();
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
