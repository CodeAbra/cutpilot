#include "cutpilot/core/NodeGraph.h"

#include <QUuid>
#include <QtGlobal>

namespace cutpilot::core {

QString NodeGraph::mintUid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

int NodeGraph::addNode(const Node &node)
{
    Node copy = node;
    copy.id = m_nextId++;
    // A uid names exactly one node: a prototype arriving without one, or
    // carrying one this graph already holds (a placed copy), gets its own.
    if (copy.uid.isEmpty() || nodeByUid(copy.uid))
        copy.uid = mintUid();
    m_nodes.push_back(copy);
    return copy.id;
}

Node *NodeGraph::nodeByUid(const QString &uid)
{
    if (uid.isEmpty())
        return nullptr;
    for (Node &n : m_nodes) {
        if (n.uid == uid)
            return &n;
    }
    return nullptr;
}

const Node *NodeGraph::nodeByUid(const QString &uid) const
{
    return const_cast<NodeGraph *>(this)->nodeByUid(uid);
}

Node *NodeGraph::nodeById(int id)
{
    for (Node &n : m_nodes) {
        if (n.id == id)
            return &n;
    }
    return nullptr;
}

const Node *NodeGraph::nodeById(int id) const
{
    for (const Node &n : m_nodes) {
        if (n.id == id)
            return &n;
    }
    return nullptr;
}

int NodeGraph::hitTest(const QPointF &world) const
{
    // Iterate back-to-front so the top-most node wins. Z-order is insertion order:
    // nothing reorders the list on select or drag, so the last-added node is on top.
    for (int i = m_nodes.size() - 1; i >= 0; --i) {
        if (m_nodes[i].containsWorld(world))
            return m_nodes[i].id;
    }
    return -1;
}

bool NodeGraph::selectOnly(int id)
{
    bool changed = false;
    for (Node &n : m_nodes) {
        const bool shouldSelect = (n.id == id);
        if (n.selected != shouldSelect) {
            n.selected = shouldSelect;
            changed = true;
        }
    }
    return changed;
}

bool NodeGraph::anySelected() const
{
    for (const Node &n : m_nodes) {
        if (n.selected)
            return true;
    }
    return false;
}

int NodeGraph::selectedId() const
{
    for (const Node &n : m_nodes) {
        if (n.selected)
            return n.id;
    }
    return -1;
}

void NodeGraph::setSelected(int id, bool selected)
{
    if (Node *n = nodeById(id))
        n->selected = selected;
}

void NodeGraph::toggleSelected(int id)
{
    if (Node *n = nodeById(id))
        n->selected = !n->selected;
}

void NodeGraph::clearSelection()
{
    for (Node &n : m_nodes)
        n.selected = false;
}

QVector<int> NodeGraph::selectedIds() const
{
    QVector<int> ids;
    for (const Node &n : m_nodes) {
        if (n.selected)
            ids.push_back(n.id);
    }
    return ids;
}

void NodeGraph::selectInRect(const QRectF &worldRect, bool additive)
{
    const QRectF band = worldRect.normalized();
    for (Node &n : m_nodes) {
        const QRectF r = n.worldRect();
        // Inclusive overlap on all four edges: a purely horizontal or vertical drag
        // makes a zero-area band, and QRectF::intersects reports false for an empty
        // rect, so a thin band would select nothing. This still selects the nodes it
        // crosses.
        const bool hit = band.left() <= r.right() && band.right() >= r.left()
            && band.top() <= r.bottom() && band.bottom() >= r.top();
        if (hit)
            n.selected = true;
        else if (!additive)
            n.selected = false;
    }
}

void NodeGraph::moveNodeTo(int id, const QPointF &worldPos)
{
    if (Node *n = nodeById(id))
        n->worldPos = worldPos;
}

void NodeGraph::moveNodesBy(const QVector<int> &ids, const QPointF &delta)
{
    for (int id : ids) {
        if (Node *n = nodeById(id))
            n->worldPos += delta;
    }
}

void NodeGraph::removeNode(int id)
{
    const int index = indexOfId(id);
    if (index != kNoIndex)
        m_nodes.removeAt(index);
}

int NodeGraph::indexOfId(int id) const
{
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].id == id)
            return i;
    }
    return kNoIndex;
}

void NodeGraph::insertNode(int index, const Node &node)
{
    const int clamped = qBound(0, index, int(m_nodes.size()));
    m_nodes.insert(clamped, node);
    // Keep the next-id counter past any restored id so a later add never collides.
    if (node.id >= m_nextId)
        m_nextId = node.id + 1;
}

void NodeGraph::raiseToTop(int id)
{
    const int index = indexOfId(id);
    if (index == kNoIndex)
        return;
    const Node node = m_nodes.takeAt(index);
    m_nodes.push_back(node);
}

void NodeGraph::raiseToTop(const QVector<int> &ids)
{
    // Pull the listed nodes out in current list order so the raised set keeps its
    // relative z-order, then re-append them above the untouched nodes.
    QVector<Node> raised;
    for (int i = 0; i < m_nodes.size();) {
        if (ids.contains(m_nodes[i].id)) {
            raised.push_back(m_nodes[i]);
            m_nodes.removeAt(i);
        } else {
            ++i;
        }
    }
    m_nodes += raised;
}

bool NodeGraph::connectionEndpointsValid(const Connection &connection) const
{
    const Node *from = nodeById(connection.fromNodeId);
    const Node *to = nodeById(connection.toNodeId);
    return from && to && connection.fromPortIndex >= 0
        && connection.fromPortIndex < from->ports.size()
        && connection.toPortIndex >= 0
        && connection.toPortIndex < to->ports.size();
}

int NodeGraph::addConnection(const Connection &connection)
{
    if (!connectionEndpointsValid(connection))
        return -1;
    Connection copy = connection;
    copy.id = m_nextConnectionId++;
    m_connections.push_back(copy);
    return copy.id;
}

Connection *NodeGraph::connectionById(int id)
{
    for (Connection &c : m_connections) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

const Connection *NodeGraph::connectionById(int id) const
{
    for (const Connection &c : m_connections) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

int NodeGraph::connectionIndexOfId(int id) const
{
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].id == id)
            return i;
    }
    return kNoIndex;
}

void NodeGraph::removeConnection(int id)
{
    const int index = connectionIndexOfId(id);
    if (index != kNoIndex)
        m_connections.removeAt(index);
}

void NodeGraph::insertConnection(int index, const Connection &connection)
{
    const int clamped = qBound(0, index, int(m_connections.size()));
    m_connections.insert(clamped, connection);
    // Keep the next-id counter past any restored id so a later add never collides.
    if (connection.id >= m_nextConnectionId)
        m_nextConnectionId = connection.id + 1;
}

int NodeGraph::connectionAtInput(int nodeId, int portIndex) const
{
    for (const Connection &c : m_connections) {
        if (c.toNodeId == nodeId && c.toPortIndex == portIndex)
            return c.id;
    }
    return -1;
}

QVector<int> NodeGraph::connectionIdsForNode(int nodeId) const
{
    QVector<int> ids;
    for (const Connection &c : m_connections) {
        if (c.touchesNode(nodeId))
            ids.push_back(c.id);
    }
    return ids;
}

} // namespace cutpilot::core
