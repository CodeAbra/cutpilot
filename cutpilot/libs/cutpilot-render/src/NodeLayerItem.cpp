#include "cutpilot/render/NodeLayerItem.h"
#include "NodeGeometryBuilder.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
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
    auto *geometryNode = static_cast<QSGGeometryNode *>(oldNode);
    if (!geometryNode) {
        geometryNode = new QSGGeometryNode;
        auto *material = new QSGVertexColorMaterial;
        geometryNode->setMaterial(material);
        geometryNode->setFlag(QSGNode::OwnsMaterial, true);
        geometryNode->setFlag(QSGNode::OwnsGeometry, true);
    }

    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;

    NodeGeometryBuilder builder;
    builder.build(m_graph.nodes(), m_theme, zoom, panLogical());

    const auto &verts = builder.vertices();
    const auto &idx = builder.indices();

    auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                     verts.size(), idx.size());
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);

    auto *vd = geometry->vertexDataAsColoredPoint2D();
    for (int i = 0; i < verts.size(); ++i) {
        const auto &v = verts[i];
        vd[i].set(v.x, v.y, v.r, v.g, v.b, v.a);
    }

    quint16 *id = geometry->indexDataAsUShort();
    for (int i = 0; i < idx.size(); ++i)
        id[i] = idx[i];

    geometryNode->setGeometry(geometry);
    geometryNode->markDirty(QSGNode::DirtyGeometry);

    return geometryNode;
}

void NodeLayerItem::mousePressEvent(QMouseEvent *event)
{
    if (!m_controller) {
        event->ignore();
        return;
    }
    forceActiveFocus();

    // Middle-button, or Space + left-button, pans the canvas — the same gestures the
    // empty canvas offered before nodes existed, now handled on the top layer so a
    // node hit and a pan never fight over the same press.
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

    if (changed)
        update();
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
