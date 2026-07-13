#include "cutpilot/render/MinimapItem.h"
#include "NodeGeometryBuilder.h"

#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

namespace cutpilot::render {

namespace {

// Inner padding around the framed board, the smallest visible block edge, and
// the stroke weights, all in the item's logical pixels.
constexpr qreal kPadding = 8.0;
constexpr qreal kMinBlockEdge = 2.0;
constexpr qreal kConnectorWidth = 1.0;
constexpr qreal kViewportStroke = 1.5;

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

// A rectangle outline as four thin quads.
void appendRectStroke(NodeGeometryBuilder::Mesh &mesh, const QRectF &rect,
                      qreal width, const QColor &color)
{
    NodeGeometryBuilder::appendQuad(
        mesh, QRectF(rect.left(), rect.top(), rect.width(), width), color);
    NodeGeometryBuilder::appendQuad(
        mesh, QRectF(rect.left(), rect.bottom() - width, rect.width(), width),
        color);
    NodeGeometryBuilder::appendQuad(
        mesh, QRectF(rect.left(), rect.top(), width, rect.height()), color);
    NodeGeometryBuilder::appendQuad(
        mesh, QRectF(rect.right() - width, rect.top(), width, rect.height()),
        color);
}

} // namespace

MinimapItem::MinimapItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
}

void MinimapItem::setLayer(NodeLayerItem *layer)
{
    if (m_layer == layer)
        return;
    if (m_layer)
        m_layer->disconnect(this);
    m_layer = layer;
    if (m_layer) {
        connect(m_layer, &NodeLayerItem::boardChanged, this,
                [this] { update(); });
    }
    emit layerChanged();
    update();
}

void MinimapItem::setController(CanvasController *controller)
{
    if (m_controller == controller)
        return;
    if (m_controller)
        m_controller->disconnect(this);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &CanvasController::cameraChanged, this,
                [this] { update(); });
    }
    emit controllerChanged();
    update();
}

void MinimapItem::setTheme(theme::Theme themeId)
{
    if (m_theme.theme() == themeId)
        return;
    m_theme.setTheme(themeId);
    update();
}

qreal MinimapItem::devicePixelRatio() const
{
    return window() ? window()->effectiveDevicePixelRatio() : 1.0;
}

QRectF MinimapItem::viewportWorldRect() const
{
    if (!m_controller || !m_layer)
        return QRectF();
    const qreal dpr = devicePixelRatio();
    const QPointF topLeft = m_controller->worldFromScreen(QPointF(0, 0), dpr);
    const QPointF bottomRight = m_controller->worldFromScreen(
        QPointF(m_layer->width(), m_layer->height()) * dpr, dpr);
    return QRectF(topLeft, bottomRight).normalized();
}

MinimapProjection MinimapItem::projection() const
{
    // The board and the camera's viewport are both always in frame, so the
    // view rectangle can never leave the minimap.
    QRectF bounds = m_layer ? m_layer->contentWorldBounds() : QRectF();
    const QRectF viewport = viewportWorldRect();
    if (bounds.isNull())
        bounds = viewport;
    else if (!viewport.isNull())
        bounds = bounds.united(viewport);
    return MinimapProjection::fit(bounds, QSizeF(width(), height()), kPadding);
}

QRectF MinimapItem::blockRectFor(int nodeId) const
{
    if (!m_layer)
        return QRectF();
    const core::Node *node = m_layer->graph().nodeById(nodeId);
    if (!node)
        return QRectF();
    QRectF block = projection().miniFromWorld(node->worldRect());
    if (block.width() < kMinBlockEdge)
        block.setWidth(kMinBlockEdge);
    if (block.height() < kMinBlockEdge)
        block.setHeight(kMinBlockEdge);
    return block;
}

QRectF MinimapItem::viewportMiniRect() const
{
    return projection().miniFromWorld(viewportWorldRect());
}

QPointF MinimapItem::worldAtItemPos(const QPointF &itemPos) const
{
    return projection().worldFromMini(itemPos);
}

QSGNode *MinimapItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGGeometryNode *>(oldNode);
    if (!node) {
        node = new QSGGeometryNode;
        auto *material = new QSGVertexColorMaterial;
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial, true);
        node->setFlag(QSGNode::OwnsGeometry, true);
    }

    NodeGeometryBuilder::Mesh mesh;
    const QRectF surface(0, 0, width(), height());

    QColor backdrop = m_theme.surfaceOverlay();
    backdrop.setAlpha(235);
    NodeGeometryBuilder::appendQuad(mesh, surface, backdrop);
    appendRectStroke(mesh, surface, 1.0, m_theme.borderSubtle());

    if (m_layer && m_controller) {
        const MinimapProjection map = projection();

        // Faint connector lines under the blocks.
        const core::NodeGraph &graph = m_layer->graph();
        for (const core::Connection &c : graph.connections()) {
            const core::Node *from = graph.nodeById(c.fromNodeId);
            const core::Node *to = graph.nodeById(c.toNodeId);
            if (!from || !to || c.fromPortIndex >= from->ports.size()
                || c.toPortIndex >= to->ports.size())
                continue;
            NodeGeometryBuilder::appendLineQuad(
                mesh, map.miniFromWorld(from->portWorldPosition(c.fromPortIndex)),
                map.miniFromWorld(to->portWorldPosition(c.toPortIndex)),
                kConnectorWidth, m_theme.borderDefault());
        }

        // Node blocks: frames first as translucent regions, then every card
        // tinted by its media's average color.
        for (const core::Node &n : graph.nodes()) {
            if (n.kind != core::NodeKind::Frame)
                continue;
            NodeGeometryBuilder::appendQuad(mesh, blockRectFor(n.id),
                                            m_theme.selectionFill());
        }
        for (const core::Node &n : graph.nodes()) {
            if (n.kind == core::NodeKind::Frame)
                continue;
            const QColor average = m_layer->nodeAverageColor(n.id);
            NodeGeometryBuilder::appendQuad(
                mesh, blockRectFor(n.id),
                average.isValid() ? average : m_theme.nodeHeader());
        }

        appendRectStroke(mesh, viewportMiniRect(), kViewportStroke,
                         m_theme.textSecondary());
    }

    uploadMesh(node, mesh);
    return node;
}

void MinimapItem::jumpTo(const QPointF &itemPos)
{
    if (!m_controller || !m_layer)
        return;
    const qreal dpr = devicePixelRatio();
    // The drag keeps the projection captured at the press: recentering moves
    // the viewport, which would otherwise shift the mapping under the cursor
    // mid-gesture.
    m_controller->centerOnWorld(
        m_dragProjection.worldFromMini(itemPos),
        QSizeF(m_layer->width(), m_layer->height()) * dpr, dpr);
}

void MinimapItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_navigating = true;
    m_dragProjection = projection();
    jumpTo(event->position());
    event->accept();
}

void MinimapItem::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_navigating) {
        event->ignore();
        return;
    }
    jumpTo(event->position());
    event->accept();
}

void MinimapItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_navigating) {
        event->ignore();
        return;
    }
    m_navigating = false;
    event->accept();
}

} // namespace cutpilot::render
