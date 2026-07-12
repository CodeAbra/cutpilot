#include <QtTest/QtTest>

#include <QSet>

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

    void setAndToggleAndClearSelection();
    void rectSelectIntersectionPlainAndAdditive();
    void thinBandSelectsNodesItCrosses();
    void removeAndIndexOf();
    void insertPreservesIdAndKeepsNextIdAhead();
    void removeRestoreRoundTrip();
    void groupMoveTranslatesEach();
    void raiseSingleAndSetWinsHitTest();
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

void TstNodeGraph::setAndToggleAndClearSelection()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(400, 0)));
    const int c = graph.addNode(makeNode(QPointF(800, 0)));

    graph.setSelected(a, true);
    graph.setSelected(c, true);
    QCOMPARE(graph.selectedIds(), (QVector<int>{ a, c }));
    QVERIFY(!graph.nodeById(b)->selected);

    graph.toggleSelected(a); // a off
    graph.toggleSelected(b); // b on
    QCOMPARE(graph.selectedIds(), (QVector<int>{ b, c }));

    graph.clearSelection();
    QVERIFY(graph.selectedIds().isEmpty());
    QVERIFY(!graph.anySelected());
}

void TstNodeGraph::rectSelectIntersectionPlainAndAdditive()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0), QSizeF(100, 100)));
    const int b = graph.addNode(makeNode(QPointF(300, 0), QSizeF(100, 100)));
    const int c = graph.addNode(makeNode(QPointF(600, 0), QSizeF(100, 100)));

    // A band overlapping only a and b.
    graph.selectInRect(QRectF(50, 50, 300, 20), false);
    QCOMPARE(graph.selectedIds(), (QVector<int>{ a, b }));

    // A plain rect select of only c deselects a and b.
    graph.selectInRect(QRectF(650, 50, 20, 20), false);
    QCOMPARE(graph.selectedIds(), (QVector<int>{ c }));

    // Additive keeps c and adds a.
    graph.selectInRect(QRectF(10, 10, 20, 20), true);
    QCOMPARE(graph.selectedIds(), (QVector<int>{ a, c }));
}

void TstNodeGraph::thinBandSelectsNodesItCrosses()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0), QSizeF(100, 100)));
    const int b = graph.addNode(makeNode(QPointF(300, 0), QSizeF(100, 100)));

    // A zero-height horizontal band drawn across both cards still selects them.
    graph.selectInRect(QRectF(QPointF(50, 50), QPointF(350, 50)), false);
    QCOMPARE(graph.selectedIds(), (QVector<int>{ a, b }));

    // A zero-area band in empty space selects nothing.
    graph.selectInRect(QRectF(QPointF(1000, 1000), QPointF(1000, 1000)), false);
    QVERIFY(graph.selectedIds().isEmpty());
}

void TstNodeGraph::removeAndIndexOf()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(400, 0)));

    QCOMPARE(graph.indexOfId(a), 0);
    QCOMPARE(graph.indexOfId(b), 1);

    graph.removeNode(a);
    QCOMPARE(graph.nodeById(a), nullptr);
    QCOMPARE(graph.indexOfId(a), NodeGraph::kNoIndex);
    QVERIFY(graph.nodeById(b) != nullptr);
    QCOMPARE(graph.indexOfId(b), 0);
}

void TstNodeGraph::insertPreservesIdAndKeepsNextIdAhead()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(400, 0)));

    Node restored = *graph.nodeById(b);
    graph.removeNode(b);
    graph.insertNode(1, restored);

    QVERIFY(graph.nodeById(b) != nullptr);
    QCOMPARE(graph.indexOfId(b), 1);

    // A later add must not reissue the restored id.
    const int next = graph.addNode(makeNode(QPointF(800, 0)));
    QVERIFY(next != a);
    QVERIFY(next != b);
}

void TstNodeGraph::removeRestoreRoundTrip()
{
    NodeGraph graph;
    graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(400, 0)));
    graph.addNode(makeNode(QPointF(800, 0)));

    const int index = graph.indexOfId(b);
    const Node snapshot = *graph.nodeById(b);
    graph.removeNode(b);
    graph.insertNode(index, snapshot);

    QCOMPARE(graph.indexOfId(b), index);
    // No duplicate ids afterward.
    QSet<int> ids;
    for (const Node &n : graph.nodes()) {
        QVERIFY(!ids.contains(n.id));
        ids.insert(n.id);
    }
}

void TstNodeGraph::groupMoveTranslatesEach()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0)));
    const int b = graph.addNode(makeNode(QPointF(400, 200)));
    const int c = graph.addNode(makeNode(QPointF(800, 0)));

    graph.moveNodesBy({ a, c }, QPointF(50, -25));
    QCOMPARE(graph.nodeById(a)->worldPos, QPointF(50, -25));
    QCOMPARE(graph.nodeById(c)->worldPos, QPointF(850, -25));
    QCOMPARE(graph.nodeById(b)->worldPos, QPointF(400, 200)); // untouched
}

void TstNodeGraph::raiseSingleAndSetWinsHitTest()
{
    NodeGraph graph;
    const int a = graph.addNode(makeNode(QPointF(0, 0), QSizeF(200, 200)));
    const int b = graph.addNode(makeNode(QPointF(0, 0), QSizeF(200, 200)));
    const int c = graph.addNode(makeNode(QPointF(0, 0), QSizeF(200, 200)));

    // c is on top; raising a puts it above c.
    QCOMPARE(graph.hitTest(QPointF(100, 100)), c);
    graph.raiseToTop(a);
    QCOMPARE(graph.hitTest(QPointF(100, 100)), a);

    // Raising a set keeps relative order and lands above the untouched node.
    graph.raiseToTop(QVector<int>{ b, c });
    const QVector<Node> &order = graph.nodes();
    QCOMPARE(order.last().id, c);          // c was after b in the list
    QCOMPARE(order[order.size() - 2].id, b);
    QCOMPARE(order.first().id, a);         // untouched a sinks to the bottom
}

QTEST_APPLESS_MAIN(TstNodeGraph)
#include "tst_nodegraph.moc"
