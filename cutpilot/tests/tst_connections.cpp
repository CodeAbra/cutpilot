#include <QtTest/QtTest>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/PortRules.h"
#include "cutpilot/core/command/AddConnectedNodeCommand.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/core/command/CompositeCommand.h"
#include "cutpilot/core/command/ConnectCommand.h"
#include "cutpilot/core/command/DeleteNodesCommand.h"
#include "cutpilot/core/command/DisconnectCommand.h"

using namespace cutpilot::core;

namespace {

// A node with one input and one output of the given types, sized and placed so
// port positions are predictable.
Node makeNode(PortType inputType, PortType outputType,
              const QPointF &pos = QPointF(0, 0))
{
    Node n;
    n.worldPos = pos;
    n.worldSize = QSizeF(200, 100);
    n.ports = {
        { QStringLiteral("in"), inputType, true, 0.5 },
        { QStringLiteral("out"), outputType, false, 0.5 },
    };
    return n;
}

Connection edge(int fromNode, int toNode, int fromPort = 1, int toPort = 0)
{
    Connection c;
    c.fromNodeId = fromNode;
    c.fromPortIndex = fromPort;
    c.toNodeId = toNode;
    c.toPortIndex = toPort;
    return c;
}

} // namespace

class TstConnections : public QObject {
    Q_OBJECT

private slots:
    void portMatchMatrix();

    void addConnectionAssignsIds();
    void connectionLookupAndRemoval();
    void insertConnectionPreservesIdAndKeepsCounterAhead();
    void connectionAtInputAndPerNodeIds();

    void connectDoUndoRedoPreservesId();
    void connectReplacesOccupiedInputAndUndoRestoresIt();
    void disconnectDoUndoRoundTrip();
    void reconnectCompositeIsOneUndoStep();
    void addConnectedNodeWiresBothDirections();
    void deleteNodeRemovesItsConnectionsAndUndoRestoresThem();
    void deleteBothEndpointsRestoresEdgeOnce();
};

void TstConnections::portMatchMatrix()
{
    // Exact type matches flow directly.
    QCOMPARE(portMatch(PortType::Image, PortType::Image), PortMatch::Direct);
    QCOMPARE(portMatch(PortType::Video, PortType::Video), PortMatch::Direct);
    QCOMPARE(portMatch(PortType::Control, PortType::Control), PortMatch::Direct);

    // Any accepts and feeds every data type.
    QCOMPARE(portMatch(PortType::Image, PortType::Any), PortMatch::Direct);
    QCOMPARE(portMatch(PortType::Any, PortType::Text), PortMatch::Direct);

    // Permitted implicit conversions, one direction only.
    QCOMPARE(portMatch(PortType::Mask, PortType::Image), PortMatch::Converted);
    QCOMPARE(portMatch(PortType::Number, PortType::Text), PortMatch::Converted);
    QCOMPARE(portMatch(PortType::Image, PortType::Mask), PortMatch::Incompatible);
    QCOMPARE(portMatch(PortType::Text, PortType::Number), PortMatch::Incompatible);

    // Unrelated data types refuse.
    QCOMPARE(portMatch(PortType::Image, PortType::Text), PortMatch::Incompatible);
    QCOMPARE(portMatch(PortType::Audio, PortType::Video), PortMatch::Incompatible);

    // Control flow never mixes with data, Any included.
    QCOMPARE(portMatch(PortType::Control, PortType::Image), PortMatch::Incompatible);
    QCOMPARE(portMatch(PortType::Text, PortType::Control), PortMatch::Incompatible);
    QCOMPARE(portMatch(PortType::Control, PortType::Any), PortMatch::Incompatible);
    QCOMPARE(portMatch(PortType::Any, PortType::Control), PortMatch::Incompatible);
}

void TstConnections::addConnectionAssignsIds()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));

    const int c1 = graph.addConnection(edge(a, b));
    const int c2 = graph.addConnection(edge(b, a));
    QVERIFY(c1 > 0);
    QVERIFY(c2 > c1); // ids are unique and increasing
    QCOMPARE(graph.connections().size(), 2);
}

void TstConnections::connectionLookupAndRemoval()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int id = graph.addConnection(edge(a, b));

    const Connection *c = graph.connectionById(id);
    QVERIFY(c);
    QCOMPARE(c->fromNodeId, a);
    QCOMPARE(c->toNodeId, b);
    QCOMPARE(graph.connectionIndexOfId(id), 0);

    graph.removeConnection(id);
    QVERIFY(!graph.connectionById(id));
    QCOMPARE(graph.connectionIndexOfId(id), NodeGraph::kNoIndex);
    QCOMPARE(graph.connections().size(), 0);
}

void TstConnections::insertConnectionPreservesIdAndKeepsCounterAhead()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));

    Connection c = edge(a, b);
    c.id = 41;
    graph.insertConnection(0, c);
    QVERIFY(graph.connectionById(41));

    // A later add must never reissue the restored id.
    const int next = graph.addConnection(edge(b, a));
    QVERIFY(next > 41);
}

void TstConnections::connectionAtInputAndPerNodeIds()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int c = graph.addNode(makeNode(PortType::Image, PortType::Image));

    const int ab = graph.addConnection(edge(a, b));
    const int bc = graph.addConnection(edge(b, c));

    QCOMPARE(graph.connectionAtInput(b, 0), ab);
    QCOMPARE(graph.connectionAtInput(c, 0), bc);
    QCOMPARE(graph.connectionAtInput(a, 0), -1);

    QCOMPARE(graph.connectionIdsForNode(b), (QVector<int>{ ab, bc }));
    QCOMPARE(graph.connectionIdsForNode(a), (QVector<int>{ ab }));
}

void TstConnections::connectDoUndoRedoPreservesId()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));

    stack.push(std::make_unique<ConnectCommand>(edge(a, b)), graph);
    QCOMPARE(graph.connections().size(), 1);
    const int id = graph.connections().first().id;

    stack.undo(graph);
    QCOMPARE(graph.connections().size(), 0);

    stack.redo(graph);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().id, id); // same edge, same id
}

void TstConnections::connectReplacesOccupiedInputAndUndoRestoresIt()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int target = graph.addNode(makeNode(PortType::Image, PortType::Image));

    stack.push(std::make_unique<ConnectCommand>(edge(a, target)), graph);
    const int firstId = graph.connectionAtInput(target, 0);

    // Connecting a second source to the same input replaces the occupant.
    stack.push(std::make_unique<ConnectCommand>(edge(b, target)), graph);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().fromNodeId, b);

    // Undo brings the original edge back with its id; redo replaces it again.
    stack.undo(graph);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connectionAtInput(target, 0), firstId);
    QCOMPARE(graph.connections().first().fromNodeId, a);

    stack.redo(graph);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().fromNodeId, b);
}

void TstConnections::disconnectDoUndoRoundTrip()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int id = graph.addConnection(edge(a, b));

    stack.push(std::make_unique<DisconnectCommand>(id), graph);
    QCOMPARE(graph.connections().size(), 0);

    stack.undo(graph);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().id, id);
    QCOMPARE(graph.connections().first().fromNodeId, a);

    stack.redo(graph);
    QCOMPARE(graph.connections().size(), 0);
}

void TstConnections::reconnectCompositeIsOneUndoStep()
{
    NodeGraph graph;
    CommandStack stack;
    const int src = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int first = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int second = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int id = graph.addConnection(edge(src, first));

    // Re-route the edge from first to second as one gesture.
    auto composite = std::make_unique<CompositeCommand>();
    composite->add(std::make_unique<DisconnectCommand>(id));
    composite->add(std::make_unique<ConnectCommand>(edge(src, second)));
    stack.push(std::move(composite), graph);

    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().toNodeId, second);

    // One undo returns the original wiring, id included.
    stack.undo(graph);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().toNodeId, first);
    QCOMPARE(graph.connections().first().id, id);
    QVERIFY(!stack.canUndo() || stack.canRedo()); // exactly one step was pushed

    stack.redo(graph);
    QCOMPARE(graph.connections().first().toNodeId, second);
}

void TstConnections::addConnectedNodeWiresBothDirections()
{
    NodeGraph graph;
    CommandStack stack;
    const int source = graph.addNode(makeNode(PortType::Text, PortType::Image));

    // Anchor at the source's output: the new node receives.
    stack.push(std::make_unique<AddConnectedNodeCommand>(
                   makeNode(PortType::Image, PortType::Image), source, 1, true, 0),
               graph);
    QCOMPARE(graph.nodes().size(), 2);
    QCOMPARE(graph.connections().size(), 1);
    const Connection made = graph.connections().first();
    QCOMPARE(made.fromNodeId, source);
    const int addedId = made.toNodeId;
    QVERIFY(graph.nodeById(addedId));

    // One undo removes both the edge and the node.
    stack.undo(graph);
    QCOMPARE(graph.nodes().size(), 1);
    QCOMPARE(graph.connections().size(), 0);

    // Redo restores the same node id and the same edge.
    stack.redo(graph);
    QVERIFY(graph.nodeById(addedId));
    QCOMPARE(graph.connections().first().toNodeId, addedId);

    // Anchor at an input: the new node feeds it.
    stack.push(std::make_unique<AddConnectedNodeCommand>(
                   makeNode(PortType::Image, PortType::Text), source, 0, false, 1),
               graph);
    QCOMPARE(graph.connections().size(), 2);
    const Connection fed = graph.connections().last();
    QCOMPARE(fed.toNodeId, source);
    QCOMPARE(fed.toPortIndex, 0);
}

void TstConnections::deleteNodeRemovesItsConnectionsAndUndoRestoresThem()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int c = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int ab = graph.addConnection(edge(a, b));
    const int bc = graph.addConnection(edge(b, c));

    stack.push(std::make_unique<DeleteNodesCommand>(QVector<int>{ b }), graph);
    QCOMPARE(graph.nodes().size(), 2);
    QCOMPARE(graph.connections().size(), 0); // both edges touched b

    stack.undo(graph);
    QCOMPARE(graph.nodes().size(), 3);
    QCOMPARE(graph.connections().size(), 2);
    QCOMPARE(graph.connectionAtInput(b, 0), ab);
    QCOMPARE(graph.connectionAtInput(c, 0), bc);

    stack.redo(graph);
    QCOMPARE(graph.connections().size(), 0);
}

void TstConnections::deleteBothEndpointsRestoresEdgeOnce()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(PortType::Text, PortType::Image));
    const int b = graph.addNode(makeNode(PortType::Image, PortType::Image));
    const int id = graph.addConnection(edge(a, b));

    stack.push(std::make_unique<DeleteNodesCommand>(QVector<int>{ a, b }), graph);
    QCOMPARE(graph.nodes().size(), 0);
    QCOMPARE(graph.connections().size(), 0);

    stack.undo(graph);
    QCOMPARE(graph.nodes().size(), 2);
    QCOMPARE(graph.connections().size(), 1); // captured once, restored once
    QCOMPARE(graph.connections().first().id, id);
}

QTEST_APPLESS_MAIN(TstConnections)
#include "tst_connections.moc"
