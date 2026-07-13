#pragma once

#include "cutpilot/core/Connection.h"
#include "cutpilot/core/Node.h"

#include <QPointF>
#include <QRectF>
#include <QVector>

namespace cutpilot::core {

// The nodes and connections on the canvas and the operations the canvas performs
// on them: add, hit-test a world point, select one or many, move, remove and
// restore, connect and disconnect, and raise to the top of the z-order.
// Hit-testing returns the top-most node under a point, where z-order is list
// order: the last node in the list wins, and touching a node raises it to the top.
// The graph holds plain values so the model is trivially testable and carries no
// rendering or Qt-GUI dependency beyond geometry types.
class NodeGraph {
public:
    // The sentinel indexOfId returns for an absent id.
    static constexpr int kNoIndex = -1;

    int addNode(const Node &node);

    const QVector<Node> &nodes() const { return m_nodes; }
    QVector<Node> &nodes() { return m_nodes; }

    Node *nodeById(int id);
    const Node *nodeById(int id) const;

    // The node carrying the durable document identity, or null (always null
    // for an empty uid). Uniqueness is upheld by addNode and the document
    // loader, so one uid resolves to at most one node.
    Node *nodeByUid(const QString &uid);
    const Node *nodeByUid(const QString &uid) const;

    // A fresh durable identity for a node's uid field.
    static QString mintUid();

    // The id of the top-most node containing the world point, or -1 if none.
    int hitTest(const QPointF &world) const;

    // Make exactly the given node selected (or clear selection with id == -1).
    // Returns true if the selection set changed.
    bool selectOnly(int id);

    // Multi-selection. setSelected marks one node without disturbing the rest;
    // toggleSelected flips one node's membership; clearSelection deselects all;
    // selectedIds lists the selected nodes.
    void setSelected(int id, bool selected);
    void toggleSelected(int id);
    void clearSelection();
    QVector<int> selectedIds() const;

    // Select every node whose world rect intersects the band. With additive false the
    // non-intersecting nodes are deselected; with additive true the prior selection is
    // kept and the intersecting nodes are added.
    void selectInRect(const QRectF &worldRect, bool additive = false);

    bool anySelected() const;
    int selectedId() const;

    // Move a node's top-left to a new world position.
    void moveNodeTo(int id, const QPointF &worldPos);

    // Translate every listed node's world position by delta.
    void moveNodesBy(const QVector<int> &ids, const QPointF &delta);

    // Remove the node with the given id; other nodes keep their ids.
    void removeNode(int id);

    // The node's position in the list, or kNoIndex when absent.
    int indexOfId(int id) const;

    // Re-insert a node value at a given position, preserving its id, and keep the
    // next-id counter ahead of that id so a later add never reissues it.
    void insertNode(int index, const Node &node);

    // Move a node, or a set of nodes, to the top of the z-order (end of the list)
    // without changing any id. A raised set keeps its relative order.
    void raiseToTop(int id);
    void raiseToTop(const QVector<int> &ids);

    // True when both endpoint nodes exist and both port indices land inside
    // their node's port list. Every path that creates a connection must hold
    // this; an edge to a missing node is silent model corruption.
    bool connectionEndpointsValid(const Connection &connection) const;

    // Connections. addConnection assigns the id, refusing (returns -1, adds
    // nothing) when the endpoints are not valid; removal and the id-preserving
    // re-insert mirror the node operations so an undone edit restores an edge
    // exactly as it was.
    int addConnection(const Connection &connection);

    const QVector<Connection> &connections() const { return m_connections; }

    Connection *connectionById(int id);
    const Connection *connectionById(int id) const;

    // The connection's position in the list, or kNoIndex when absent.
    int connectionIndexOfId(int id) const;

    void removeConnection(int id);

    // Re-insert a connection value at a given position, preserving its id, and keep
    // the next-id counter ahead of that id so a later add never reissues it.
    void insertConnection(int index, const Connection &connection);

    // The id of the connection feeding the given input port, or -1. An input holds
    // at most one edge; the connect command enforces this by replacement.
    int connectionAtInput(int nodeId, int portIndex) const;

    // Ids of every connection touching the node, in list order.
    QVector<int> connectionIdsForNode(int nodeId) const;

private:
    QVector<Node> m_nodes;
    int m_nextId = 1;
    QVector<Connection> m_connections;
    int m_nextConnectionId = 1;
};

} // namespace cutpilot::core
