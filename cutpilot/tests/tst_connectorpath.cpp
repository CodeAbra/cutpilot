#include <QtTest/QtTest>

#include "cutpilot/core/ConnectorPath.h"
#include "cutpilot/core/Node.h"

using namespace cutpilot::core;

namespace {

Node makePortedNode()
{
    Node n;
    n.worldPos = QPointF(100, 200);
    n.worldSize = QSizeF(200, 100);
    n.ports = {
        { QStringLiteral("image"), PortType::Image, true, 0.25 },
        { QStringLiteral("mask"), PortType::Mask, true, 0.75 },
        { QStringLiteral("result"), PortType::Image, false, 0.5 },
    };
    return n;
}

} // namespace

class TstConnectorPath : public QObject {
    Q_OBJECT

private slots:
    void controlPointsLeaveAndEnterHorizontally();
    void backwardTargetKeepsOutwardTangents();
    void sampleEndpointsAreExact();
    void sampleCountGrowsWithSpanAndClamps();
    void boundsContainEverySampleAtAnyRelativePosition();
    void portWorldPositionSitsOnTheCorrectEdge();
    void portPickingFindsNearestWithinRadius();
    void distanceIsZeroOnCurveAndGrowsOffIt();
};

void TstConnectorPath::controlPointsLeaveAndEnterHorizontally()
{
    const QPointF from(0, 0);
    const QPointF to(300, 120);
    const auto cp = connectorControlPoints(from, to);

    QCOMPARE(cp[0], from);
    QCOMPARE(cp[3], to);
    QVERIFY(cp[1].x() > from.x()); // leaves the output heading right
    QCOMPARE(cp[1].y(), from.y());
    QVERIFY(cp[2].x() < to.x()); // enters the input from the left
    QCOMPARE(cp[2].y(), to.y());
}

void TstConnectorPath::backwardTargetKeepsOutwardTangents()
{
    // The target sits far left of the source: the curve must still leave the
    // output rightward and enter the input rightward, looping around.
    const QPointF from(500, 100);
    const QPointF to(-200, 40);
    const auto cp = connectorControlPoints(from, to);

    QVERIFY(cp[1].x() > from.x());
    QVERIFY(cp[2].x() < to.x());

    // The backward reach must exceed the forward minimum so the loop clears the
    // cards rather than pinching between them.
    QVERIFY(cp[1].x() - from.x() > 48.0);
}

void TstConnectorPath::sampleEndpointsAreExact()
{
    const QPointF from(12.5, -3.25);
    const QPointF to(-410.0, 220.75);
    const QVector<QPointF> pts = sampleConnector(from, to);

    QVERIFY(pts.size() >= 2);
    QCOMPARE(pts.first(), from);
    QCOMPARE(pts.last(), to);
}

void TstConnectorPath::sampleCountGrowsWithSpanAndClamps()
{
    const int shortCount = sampleConnector(QPointF(0, 0), QPointF(10, 0)).size();
    const int longCount = sampleConnector(QPointF(0, 0), QPointF(4000, 900)).size();

    QVERIFY(longCount > shortCount);
    QCOMPARE(shortCount, 13); // the smallest polyline: the clamp floor plus one
    QCOMPARE(longCount, 49);  // the largest polyline: the clamp ceiling plus one
}

void TstConnectorPath::boundsContainEverySampleAtAnyRelativePosition()
{
    const QVector<QPointF> targets = {
        QPointF(400, 0),    // straight right
        QPointF(400, 300),  // down-right
        QPointF(-350, -80), // backward
        QPointF(0, 250),    // straight down
        QPointF(-10, -10),  // nearly on top of the source
    };
    const QPointF from(0, 0);
    const qreal pad = 4.0;

    for (const QPointF &to : targets) {
        const QRectF bounds = connectorBounds(from, to, pad);
        for (const QPointF &p : sampleConnector(from, to))
            QVERIFY(bounds.contains(p));
    }
}

void TstConnectorPath::portWorldPositionSitsOnTheCorrectEdge()
{
    const Node n = makePortedNode();

    // Inputs on the left edge at their fractional offsets.
    QCOMPARE(n.portWorldPosition(0), QPointF(100, 225));
    QCOMPARE(n.portWorldPosition(1), QPointF(100, 275));
    // Output on the right edge.
    QCOMPARE(n.portWorldPosition(2), QPointF(300, 250));
}

void TstConnectorPath::portPickingFindsNearestWithinRadius()
{
    const Node n = makePortedNode();

    // A point near the first input picks it.
    QCOMPARE(n.portIndexAtWorld(QPointF(103, 228), 8.0), 0);
    // Midway between the two inputs, the nearer one wins.
    QCOMPARE(n.portIndexAtWorld(QPointF(100, 262), 30.0), 1);
    // Near the output edge.
    QCOMPARE(n.portIndexAtWorld(QPointF(297, 251), 8.0), 2);
    // Outside every port's radius: no pick.
    QCOMPARE(n.portIndexAtWorld(QPointF(200, 250), 8.0), -1);
}

void TstConnectorPath::distanceIsZeroOnCurveAndGrowsOffIt()
{
    const QPointF from(0, 0);
    const QPointF to(400, 200);
    const QVector<QPointF> samples = cutpilot::core::sampleConnector(from, to);

    // Every sample point lies on the polyline, so its distance is zero.
    for (const QPointF &p : samples)
        QVERIFY(cutpilot::core::connectorDistance(from, to, p) < 1e-9);

    // A point offset perpendicular to the curve midpoint reads roughly that
    // offset; a far point reads far.
    const QPointF mid = samples[samples.size() / 2];
    const qreal near = cutpilot::core::connectorDistance(from, to, mid + QPointF(0.0, 9.0));
    QVERIFY(near > 4.0 && near < 10.0 + 1e-9);
    QVERIFY(cutpilot::core::connectorDistance(from, to, QPointF(200.0, -500.0)) > 300.0);
}

QTEST_APPLESS_MAIN(TstConnectorPath)
#include "tst_connectorpath.moc"
