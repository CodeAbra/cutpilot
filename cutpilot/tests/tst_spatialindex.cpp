#include <QtTest/QtTest>

#include <QSet>

#include "cutpilot/core/Node.h"
#include "cutpilot/core/SpatialIndex.h"

using namespace cutpilot::core;

namespace {

QSet<int> toSet(const QVector<int> &ids)
{
    return QSet<int>(ids.cbegin(), ids.cend());
}

// Brute-force intersection over a node list, the geometric answer the index must match.
QSet<int> bruteForce(const QVector<Node> &nodes, const QRectF &rect)
{
    QSet<int> ids;
    for (const Node &n : nodes) {
        if (n.worldRect().intersects(rect))
            ids.insert(n.id);
    }
    return ids;
}

} // namespace

class TstSpatialIndex : public QObject {
    Q_OBJECT

private slots:
    void rectQueryExactNoFalsePositives();
    void pointQueryContains();
    void removeAndUpdateKeepQueriesCorrect();
    void negativeCoordinatesAndDistantCells();
    void rebuildFromNodeList();

    void viewportWorldRectMapsKnownCamera();
    void visibleIdsEqualsBruteForceAtScale();
    void visibleCountStaysBoundedAsCameraSweeps();
    void incrementalUpdatesMatchBruteForce();
};

void TstSpatialIndex::rectQueryExactNoFalsePositives()
{
    SpatialIndex index;
    index.insert(1, QRectF(0, 0, 100, 100));
    index.insert(2, QRectF(500, 500, 100, 100));
    index.insert(3, QRectF(40, 40, 100, 100)); // overlaps node 1

    // A rect over the first cluster returns 1 and 3 but never 2, each once.
    const QVector<int> hit = index.queryRect(QRectF(10, 10, 60, 60));
    QCOMPARE(toSet(hit), (QSet<int>{ 1, 3 }));
    QCOMPARE(hit.size(), 2); // no duplicate from multi-cell residency

    // A rect in empty space returns nothing.
    QVERIFY(index.queryRect(QRectF(2000, 2000, 50, 50)).isEmpty());
}

void TstSpatialIndex::pointQueryContains()
{
    SpatialIndex index;
    index.insert(1, QRectF(0, 0, 100, 100));
    index.insert(2, QRectF(50, 50, 100, 100));

    QCOMPARE(toSet(index.queryPoint(QPointF(75, 75))), (QSet<int>{ 1, 2 }));
    QCOMPARE(toSet(index.queryPoint(QPointF(10, 10))), (QSet<int>{ 1 }));
    QVERIFY(index.queryPoint(QPointF(500, 500)).isEmpty());
}

void TstSpatialIndex::removeAndUpdateKeepQueriesCorrect()
{
    SpatialIndex index;
    index.insert(1, QRectF(0, 0, 100, 100));
    index.insert(2, QRectF(0, 0, 100, 100));

    index.remove(1);
    QCOMPARE(toSet(index.queryPoint(QPointF(50, 50))), (QSet<int>{ 2 }));

    // A moved node is found at its new place and not its old one.
    index.update(2, QRectF(1000, 1000, 100, 100));
    QVERIFY(index.queryPoint(QPointF(50, 50)).isEmpty());
    QCOMPARE(toSet(index.queryPoint(QPointF(1050, 1050))), (QSet<int>{ 2 }));
}

void TstSpatialIndex::negativeCoordinatesAndDistantCells()
{
    SpatialIndex index;
    index.insert(1, QRectF(-500, -500, 100, 100));
    index.insert(2, QRectF(5000, 5000, 100, 100));

    QCOMPARE(toSet(index.queryPoint(QPointF(-450, -450))), (QSet<int>{ 1 }));
    QCOMPARE(toSet(index.queryRect(QRectF(-520, -520, 60, 60))), (QSet<int>{ 1 }));
    // Two far-apart nodes never share a bucket: querying near one never returns the other.
    QVERIFY(!index.queryRect(QRectF(-520, -520, 60, 60)).contains(2));
}

void TstSpatialIndex::rebuildFromNodeList()
{
    QVector<Node> nodes;
    for (int i = 0; i < 5; ++i) {
        Node n;
        n.id = i + 1;
        n.worldPos = QPointF(i * 400, 0);
        n.worldSize = QSizeF(100, 100);
        nodes.push_back(n);
    }

    SpatialIndex index;
    index.rebuild(nodes);
    QCOMPARE(index.count(), 5);
    QCOMPARE(toSet(index.queryPoint(QPointF(1250, 50))), (QSet<int>{ 4 }));
}

void TstSpatialIndex::viewportWorldRectMapsKnownCamera()
{
    // screenPixel = world * scale + translation, so a viewport of a known size maps to
    // a known world rect.
    const QRectF rect =
        viewportWorldRect(2.0, QPointF(100, 50), QSizeF(800, 600));
    QCOMPARE(rect, QRectF(-50, -25, 400, 300));
}

void TstSpatialIndex::visibleIdsEqualsBruteForceAtScale()
{
    // Several hundred nodes on a wide grid.
    QVector<Node> nodes;
    SpatialIndex index;
    int id = 1;
    for (int gx = 0; gx < 20; ++gx) {
        for (int gy = 0; gy < 20; ++gy) {
            Node n;
            n.id = id++;
            n.worldPos = QPointF(gx * 400.0, gy * 400.0);
            n.worldSize = QSizeF(200, 150);
            nodes.push_back(n);
            index.insert(n.id, n.worldRect());
        }
    }
    QCOMPARE(nodes.size(), 400);

    const qreal scale = 1.0;
    const QPointF translation(0, 0);
    const QSizeF viewport(1280, 800);

    const QRectF worldRect = viewportWorldRect(scale, translation, viewport);
    const QVector<int> visible = visibleIds(index, scale, translation, viewport);

    QCOMPARE(toSet(visible), bruteForce(nodes, worldRect));
    // Only a small fraction of the whole board overlaps the viewport.
    QVERIFY(visible.size() < nodes.size() / 4);
}

void TstSpatialIndex::visibleCountStaysBoundedAsCameraSweeps()
{
    QVector<Node> nodes;
    SpatialIndex index;
    int id = 1;
    for (int gx = 0; gx < 30; ++gx) {
        for (int gy = 0; gy < 30; ++gy) {
            Node n;
            n.id = id++;
            n.worldPos = QPointF(gx * 400.0, gy * 400.0);
            n.worldSize = QSizeF(200, 150);
            nodes.push_back(n);
            index.insert(n.id, n.worldRect());
        }
    }
    QCOMPARE(nodes.size(), 900);

    const qreal scale = 1.0;
    const QSizeF viewport(1280, 800);

    // Sweep the camera translation across the field; the visible count tracks the
    // viewport, never the total node count.
    for (int step = 0; step < 10; ++step) {
        const QPointF translation(-step * 900.0, -step * 700.0);
        const QVector<int> visible = visibleIds(index, scale, translation, viewport);
        const QRectF worldRect = viewportWorldRect(scale, translation, viewport);
        QCOMPARE(toSet(visible), bruteForce(nodes, worldRect));
        QVERIFY(visible.size() < 60); // viewport-bounded, far under 900
    }
}

void TstSpatialIndex::incrementalUpdatesMatchBruteForce()
{
    // A wide grid; ids run 1..400 in the same order they are pushed.
    QVector<Node> nodes;
    SpatialIndex index;
    int id = 1;
    for (int gx = 0; gx < 20; ++gx) {
        for (int gy = 0; gy < 20; ++gy) {
            Node n;
            n.id = id++;
            n.worldPos = QPointF(gx * 400.0, gy * 400.0);
            n.worldSize = QSizeF(200, 150);
            nodes.push_back(n);
            index.insert(n.id, n.worldRect());
        }
    }

    const qreal scale = 1.0;
    const QPointF translation(0, 0);
    const QSizeF viewport(1280, 800);

    // Drag a handful of nodes across many steps, updating only the moved ids each step
    // exactly as the live drag does, and confirm culling still matches brute force.
    const QVector<int> dragged = { 1, 2, 21, 22 };
    const QPointF stepDelta(37.0, -19.0);
    for (int step = 0; step < 25; ++step) {
        for (int d : dragged) {
            Node &n = nodes[d - 1];
            n.worldPos += stepDelta;
            index.update(n.id, n.worldRect());
        }
        const QRectF worldRect = viewportWorldRect(scale, translation, viewport);
        QCOMPARE(toSet(visibleIds(index, scale, translation, viewport)),
                 bruteForce(nodes, worldRect));
    }
    QCOMPARE(index.count(), nodes.size()); // no leaked or duplicated entries
}

QTEST_APPLESS_MAIN(TstSpatialIndex)
#include "tst_spatialindex.moc"
