#include <QtTest/QtTest>

#include <QSet>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/command/AddNodeCommand.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/core/command/DeleteNodesCommand.h"
#include "cutpilot/core/command/MoveNodesCommand.h"

using namespace cutpilot::core;

namespace {

Node makeNode(const QPointF &pos, const QSizeF &size = QSizeF(120, 90))
{
    Node n;
    n.worldPos = pos;
    n.worldSize = size;
    return n;
}

// A graph-free command that mutates external counters, for exercising the stack
// mechanics: it tracks how many times it was applied and a running net value.
class CountingCommand : public Command {
public:
    CountingCommand(int *appliedCount, int *net, int step)
        : m_appliedCount(appliedCount)
        , m_net(net)
        , m_step(step)
    {
    }

    void apply(NodeGraph &) override
    {
        ++*m_appliedCount;
        *m_net += m_step;
    }
    void revert(NodeGraph &) override { *m_net -= m_step; }

private:
    int *m_appliedCount;
    int *m_net;
    int m_step;
};

std::unique_ptr<Command> counting(int *applied, int *net, int step)
{
    return std::make_unique<CountingCommand>(applied, net, step);
}

} // namespace

class TstCommandStack : public QObject {
    Q_OBJECT

private slots:
    void pushAppliesAndRecords();
    void recordStoresWithoutReapplying();
    void undoRedoAndAvailability();
    void newPushTruncatesRedoTail();
    void boundedDepthDropsOldest();

    void addUndoRedoPreservesId();
    void addRaiseUndoRedoKeepsAddZOrder();
    void deleteUndoRestoresIdAndZOrder();
    void moveDoUndoRedoRoundTrip();
    void recordedDragLandsAtNetDeltaNotDouble();
    void singleDragIsOneCoalescedCommand();
};

void TstCommandStack::pushAppliesAndRecords()
{
    NodeGraph graph;
    CommandStack stack;
    int applied = 0, net = 0;

    stack.push(counting(&applied, &net, 1), graph);
    QCOMPARE(applied, 1); // push applied the command
    QCOMPARE(net, 1);
    QVERIFY(stack.canUndo());
    QVERIFY(!stack.canRedo());
    QCOMPARE(stack.depth(), 1);
}

void TstCommandStack::recordStoresWithoutReapplying()
{
    NodeGraph graph;
    CommandStack stack;
    int applied = 0, net = 0;

    net += 5; // simulate a live edit the caller already performed
    stack.record(counting(&applied, &net, 5));
    QCOMPARE(applied, 0); // record must not re-apply
    QCOMPARE(net, 5);
    QVERIFY(stack.canUndo());

    stack.undo(graph);
    QCOMPARE(net, 0);
    stack.redo(graph);
    QCOMPARE(net, 5);
    QCOMPARE(applied, 1); // redo re-applies exactly once
}

void TstCommandStack::undoRedoAndAvailability()
{
    NodeGraph graph;
    CommandStack stack;
    int applied = 0, net = 0;

    stack.push(counting(&applied, &net, 1), graph);
    stack.push(counting(&applied, &net, 10), graph);
    QCOMPARE(net, 11);

    stack.undo(graph);
    QCOMPARE(net, 1);
    QVERIFY(stack.canUndo());
    QVERIFY(stack.canRedo());

    stack.undo(graph);
    QCOMPARE(net, 0);
    QVERIFY(!stack.canUndo());

    stack.redo(graph);
    QCOMPARE(net, 1);
    stack.redo(graph);
    QCOMPARE(net, 11);
    QVERIFY(!stack.canRedo());
}

void TstCommandStack::newPushTruncatesRedoTail()
{
    NodeGraph graph;
    CommandStack stack;
    int applied = 0, net = 0;

    stack.push(counting(&applied, &net, 1), graph);
    stack.push(counting(&applied, &net, 2), graph);
    stack.undo(graph); // net back to 1, a redo tail of one exists
    QVERIFY(stack.canRedo());

    stack.push(counting(&applied, &net, 4), graph);
    QVERIFY(!stack.canRedo()); // the tail was discarded
    QCOMPARE(stack.depth(), 2);
    QCOMPARE(net, 5);
}

void TstCommandStack::boundedDepthDropsOldest()
{
    NodeGraph graph;
    CommandStack stack(3); // small cap
    int applied = 0, net = 0;

    for (int i = 0; i < 5; ++i)
        stack.push(counting(&applied, &net, 1), graph);
    QCOMPARE(net, 5);
    QCOMPARE(stack.depth(), 3); // only the last three are retained

    stack.undo(graph);
    stack.undo(graph);
    stack.undo(graph);
    QCOMPARE(net, 2);          // the two dropped entries can no longer be undone
    QVERIFY(!stack.canUndo());
    stack.undo(graph);          // no-op past the retained history
    QCOMPARE(net, 2);
}

void TstCommandStack::addUndoRedoPreservesId()
{
    NodeGraph graph;
    CommandStack stack;

    auto add = std::make_unique<AddNodeCommand>(makeNode(QPointF(100, 100)));
    AddNodeCommand *addPtr = add.get();
    stack.push(std::move(add), graph);
    const int id = addPtr->nodeId();
    QVERIFY(graph.nodeById(id) != nullptr);

    stack.undo(graph);
    QCOMPARE(graph.nodeById(id), nullptr);

    stack.redo(graph);
    QVERIFY(graph.nodeById(id) != nullptr);
    QCOMPARE(graph.nodeById(id)->id, id); // same id, not a fresh one
}

void TstCommandStack::addRaiseUndoRedoKeepsAddZOrder()
{
    NodeGraph graph;
    CommandStack stack;

    auto addA = std::make_unique<AddNodeCommand>(makeNode(QPointF(0, 0)));
    AddNodeCommand *aPtr = addA.get();
    stack.push(std::move(addA), graph);
    const int a = aPtr->nodeId();

    auto addB = std::make_unique<AddNodeCommand>(makeNode(QPointF(200, 0)));
    AddNodeCommand *bPtr = addB.get();
    stack.push(std::move(addB), graph);
    const int b = bPtr->nodeId();

    // B was added last, so it sits on top of A.
    QCOMPARE(graph.indexOfId(a), 0);
    QCOMPARE(graph.indexOfId(b), 1);

    // A raise-on-touch of A reorders z outside the command stack.
    graph.raiseToTop(QVector<int>{ a });
    QCOMPARE(graph.indexOfId(b), 0);
    QCOMPARE(graph.indexOfId(a), 1);

    // Undo then redo the add of B. Redo must restore B at the z it was added at (top),
    // not wherever the intervening raise left it.
    stack.undo(graph);
    QCOMPARE(graph.nodeById(b), nullptr);
    stack.redo(graph);
    QCOMPARE(graph.indexOfId(b), 1); // back on top, its original add index
}

void TstCommandStack::deleteUndoRestoresIdAndZOrder()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(200, 0)));
    const int c = graph.addNode(makeNode(QPointF(400, 0)));

    stack.push(std::make_unique<DeleteNodesCommand>(QVector<int>{ a, c }), graph);
    QCOMPARE(graph.nodeById(a), nullptr);
    QCOMPARE(graph.nodeById(c), nullptr);
    QVERIFY(graph.nodeById(b) != nullptr);

    stack.undo(graph);
    // Ids and z-order restored, no duplicate ids.
    QCOMPARE(graph.indexOfId(a), 0);
    QCOMPARE(graph.indexOfId(b), 1);
    QCOMPARE(graph.indexOfId(c), 2);
    QSet<int> ids;
    for (const Node &n : graph.nodes()) {
        QVERIFY(!ids.contains(n.id));
        ids.insert(n.id);
    }

    stack.redo(graph);
    QCOMPARE(graph.nodeById(a), nullptr);
    QCOMPARE(graph.nodeById(c), nullptr);
}

void TstCommandStack::moveDoUndoRedoRoundTrip()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(100, 100)));

    stack.push(std::make_unique<MoveNodesCommand>(QVector<int>{ a, b }, QPointF(30, 40)),
               graph);
    QCOMPARE(graph.nodeById(a)->worldPos, QPointF(30, 40));
    QCOMPARE(graph.nodeById(b)->worldPos, QPointF(130, 140));

    stack.undo(graph);
    QCOMPARE(graph.nodeById(a)->worldPos, QPointF(0, 0));
    QCOMPARE(graph.nodeById(b)->worldPos, QPointF(100, 100));

    stack.redo(graph);
    QCOMPARE(graph.nodeById(a)->worldPos, QPointF(30, 40));
    QCOMPARE(graph.nodeById(b)->worldPos, QPointF(130, 140));
}

void TstCommandStack::recordedDragLandsAtNetDeltaNotDouble()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const QPointF origin = graph.nodeById(a)->worldPos;
    const QPointF delta(75, -20);

    // Simulate the live drag: the layer already moved the node by the net delta.
    graph.moveNodesBy({ a }, delta);
    QCOMPARE(graph.nodeById(a)->worldPos, origin + delta);

    // Record the net-delta command; record must not re-apply.
    stack.record(std::make_unique<MoveNodesCommand>(QVector<int>{ a }, delta));
    QCOMPARE(graph.nodeById(a)->worldPos, origin + delta);        // net, not origin+2*delta
    QVERIFY(graph.nodeById(a)->worldPos != origin + delta * 2.0);

    stack.undo(graph);
    QCOMPARE(graph.nodeById(a)->worldPos, origin);                // back to the pre-drag origin
    stack.redo(graph);
    QCOMPARE(graph.nodeById(a)->worldPos, origin + delta);
}

void TstCommandStack::singleDragIsOneCoalescedCommand()
{
    NodeGraph graph;
    CommandStack stack;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));

    // A logical drag composed of several live steps; the layer applies each live and
    // records only the net delta once on release.
    const QVector<QPointF> steps = { QPointF(10, 0), QPointF(10, 5), QPointF(5, 5) };
    QPointF net(0, 0);
    for (const QPointF &s : steps) {
        graph.moveNodesBy({ a }, s);
        net += s;
    }
    stack.record(std::make_unique<MoveNodesCommand>(QVector<int>{ a }, net));
    QCOMPARE(stack.depth(), 1); // one entry for the whole gesture

    stack.undo(graph);
    QCOMPARE(graph.nodeById(a)->worldPos, QPointF(0, 0)); // one undo reverses the whole drag
}

QTEST_APPLESS_MAIN(TstCommandStack)
#include "tst_commandstack.moc"
