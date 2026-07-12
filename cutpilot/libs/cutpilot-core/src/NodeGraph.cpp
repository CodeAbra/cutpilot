#include "cutpilot/core/NodeGraph.h"

#include <QtGlobal>

namespace cutpilot::core {

int NodeGraph::addNode(const Node &node)
{
    Node copy = node;
    copy.id = m_nextId++;
    m_nodes.push_back(copy);
    return copy.id;
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
    for (Node &n : m_nodes) {
        if (n.worldRect().intersects(worldRect))
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

} // namespace cutpilot::core
