#include <QtTest/QtTest>

#include "cutpilot/core/AlignmentGuides.h"

using namespace cutpilot::core;

class TstAlignmentGuides : public QObject {
    Q_OBJECT

private slots:
    void alignedEdgeProducesGuideWithSpan();
    void horizontalAlignmentProducesGuide();
    void outsideThresholdProducesNoGuide();
    void multiNodeBoundingBoxComparesIdentically();
    void noNeighboursIsEmpty();
};

void TstAlignmentGuides::alignedEdgeProducesGuideWithSpan()
{
    const QRectF moving(100, 0, 50, 50);
    // Neighbour shares only the left edge (x == 100); different width so centre and
    // right do not align, and it is far away in y.
    const QRectF neighbour(100, 300, 200, 40);

    const auto guides = computeAlignmentGuides(moving, { neighbour }, 2.0);
    QCOMPARE(guides.size(), 1);
    QCOMPARE(guides[0].axis, GuideAxis::Vertical);
    QCOMPARE(guides[0].coordinate, 100.0);
    // Span is the union extent along y of the moving rect and the matched neighbour.
    QCOMPARE(guides[0].spanStart, 0.0);
    QCOMPARE(guides[0].spanEnd, 340.0);
}

void TstAlignmentGuides::horizontalAlignmentProducesGuide()
{
    const QRectF moving(0, 100, 50, 50);      // top == 100
    const QRectF neighbour(400, 100, 60, 30); // top == 100, far in x

    const auto guides = computeAlignmentGuides(moving, { neighbour }, 1.0);
    QCOMPARE(guides.size(), 1);
    QCOMPARE(guides[0].axis, GuideAxis::Horizontal);
    QCOMPARE(guides[0].coordinate, 100.0);
    QCOMPARE(guides[0].spanStart, 0.0);
    QCOMPARE(guides[0].spanEnd, 460.0);
}

void TstAlignmentGuides::outsideThresholdProducesNoGuide()
{
    const QRectF moving(0, 0, 50, 50);
    const QRectF neighbour(100, 100, 50, 50); // every feature differs by far more than 2

    QVERIFY(computeAlignmentGuides(moving, { neighbour }, 2.0).isEmpty());
}

void TstAlignmentGuides::multiNodeBoundingBoxComparesIdentically()
{
    // A multi-node drag passes the selection bounding box as the moving rect.
    const QRectF selectionBounds(0, 0, 300, 50);
    const QRectF neighbour(0, 400, 40, 40); // left edge aligns at x == 0

    const auto guides = computeAlignmentGuides(selectionBounds, { neighbour }, 1.0);
    QVERIFY(!guides.isEmpty());
    QCOMPARE(guides[0].axis, GuideAxis::Vertical);
    QCOMPARE(guides[0].coordinate, 0.0);
}

void TstAlignmentGuides::noNeighboursIsEmpty()
{
    QVERIFY(computeAlignmentGuides(QRectF(0, 0, 50, 50), {}, 5.0).isEmpty());
}

QTEST_APPLESS_MAIN(TstAlignmentGuides)
#include "tst_alignmentguides.moc"
