#include <QtTest/QtTest>

#include "cutpilot/core/NodeGraph.h"

using namespace cutpilot::core;

class TstNodeGraph : public QObject {
    Q_OBJECT

private slots:
    void addAssignsIncreasingIds();
};

void TstNodeGraph::addAssignsIncreasingIds()
{
    NodeGraph graph;
    Node a;
    a.worldPos = QPointF(0, 0);
    a.worldSize = QSizeF(100, 80);
    Node b = a;
    b.worldPos = QPointF(200, 0);

    const int idA = graph.addNode(a);
    const int idB = graph.addNode(b);

    QVERIFY(idB > idA);
    QCOMPARE(graph.nodes().size(), 2);
}

QTEST_APPLESS_MAIN(TstNodeGraph)
#include "tst_nodegraph.moc"
