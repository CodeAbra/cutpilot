#include <QtTest/QtTest>

#include "cutpilot/core/NodeGraph.h"

using namespace cutpilot::core;

namespace {

Node makeNode(const QPointF &pos, const QSizeF &size = QSizeF(120, 90))
{
    Node n;
    n.worldPos = pos;
    n.worldSize = size;
    return n;
}

} // namespace

class TstNodeGraph : public QObject {
    Q_OBJECT

private slots:
    void addAssignsIncreasingIds();
    void hitTestReturnsTopMostAndMiss();
    void selectOnlyMarksExactlyOne();
    void moveRepositionsWorldRect();
};

void TstNodeGraph::addAssignsIncreasingIds()
{
    NodeGraph graph;
    const int idA = graph.addNode(makeNode(QPointF(0, 0)));
    const int idB = graph.addNode(makeNode(QPointF(400, 0)));

    QVERIFY(idB > idA);
    QCOMPARE(graph.nodes().size(), 2);
}

void TstNodeGraph::hitTestReturnsTopMostAndMiss()
{
    NodeGraph graph;
    // Two overlapping cards; the later-added one is on top in insertion z-order.
    const int lower = graph.addNode(makeNode(QPointF(0, 0), QSizeF(200, 200)));
    const int upper = graph.addNode(makeNode(QPointF(50, 50), QSizeF(200, 200)));

    QCOMPARE(graph.hitTest(QPointF(100, 100)), upper);
    QCOMPARE(graph.hitTest(QPointF(10, 10)), lower);
    QCOMPARE(graph.hitTest(QPointF(1000, 1000)), -1);
}

void TstNodeGraph::selectOnlyMarksExactlyOne()
{
    NodeGraph graph;
    const int idA = graph.addNode(makeNode(QPointF(0, 0)));
    const int idB = graph.addNode(makeNode(QPointF(400, 0)));

    graph.selectOnly(idB);
    QVERIFY(graph.anySelected());
    QCOMPARE(graph.selectedId(), idB);
    QVERIFY(!graph.nodeById(idA)->selected);
    QVERIFY(graph.nodeById(idB)->selected);

    graph.selectOnly(-1);
    QVERIFY(!graph.anySelected());
}

void TstNodeGraph::moveRepositionsWorldRect()
{
    NodeGraph graph;
    const int id = graph.addNode(makeNode(QPointF(100, 100)));

    graph.moveNodeTo(id, QPointF(250, 175));
    QCOMPARE(graph.nodeById(id)->worldPos, QPointF(250, 175));
    QCOMPARE(graph.nodeById(id)->worldRect().topLeft(), QPointF(250, 175));
}

QTEST_APPLESS_MAIN(TstNodeGraph)
#include "tst_nodegraph.moc"
