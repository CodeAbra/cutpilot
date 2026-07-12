#pragma once

#include "cutpilot/core/Node.h"

#include <QPointF>
#include <QVector>

namespace cutpilot::core {

// The set of nodes on the canvas and the operations the canvas performs on them:
// add, hit-test a world point, select, and move. Hit-testing returns the top-most
// node under a point, where z-order is insertion order: the last-added node wins and
// selecting or dragging does not raise a node. The graph holds plain values so the
// model is trivially testable and carries no rendering or Qt-GUI dependency beyond
// geometry types.
class NodeGraph {
public:
    int addNode(const Node &node);

    const QVector<Node> &nodes() const { return m_nodes; }
    QVector<Node> &nodes() { return m_nodes; }

    Node *nodeById(int id);
    const Node *nodeById(int id) const;

    // The id of the top-most node containing the world point, or -1 if none.
    int hitTest(const QPointF &world) const;

    // Make exactly the given node selected (or clear selection with id == -1).
    // Returns true if the selection set changed.
    bool selectOnly(int id);

    bool anySelected() const;
    int selectedId() const;

    // Move a node's top-left to a new world position.
    void moveNodeTo(int id, const QPointF &worldPos);

private:
    QVector<Node> m_nodes;
    int m_nextId = 1;
};

} // namespace cutpilot::core
