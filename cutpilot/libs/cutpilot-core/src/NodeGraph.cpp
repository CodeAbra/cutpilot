#include "cutpilot/core/NodeGraph.h"

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
    // Iterate back-to-front so the top-most (last added / last touched) node wins.
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

void NodeGraph::moveNodeTo(int id, const QPointF &worldPos)
{
    if (Node *n = nodeById(id))
        n->worldPos = worldPos;
}

} // namespace cutpilot::core
