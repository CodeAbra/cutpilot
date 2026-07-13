#include <QtTest/QtTest>

#include "cutpilot/render/CanvasCamera.h"
#include "cutpilot/render/MinimapProjection.h"

using cutpilot::render::CanvasCamera;
using cutpilot::render::MinimapProjection;

namespace {

bool nearlyEqual(const QPointF &a, const QPointF &b, qreal epsilon = 1e-6)
{
    return qAbs(a.x() - b.x()) <= epsilon && qAbs(a.y() - b.y()) <= epsilon;
}

} // namespace

class MinimapTest : public QObject {
    Q_OBJECT

private slots:
    void projectionRoundTripsExactly()
    {
        const QRectF bounds(-350.0, 120.0, 2400.0, 900.0);
        const auto projection =
            MinimapProjection::fit(bounds, QSizeF(220.0, 150.0), 8.0);

        for (const QPointF &world :
             { bounds.topLeft(), bounds.bottomRight(), bounds.center(),
               QPointF(0.0, 0.0), QPointF(-9999.0, 4321.0) }) {
            const QPointF mini = projection.miniFromWorld(world);
            QVERIFY(nearlyEqual(projection.worldFromMini(mini), world, 1e-6));
        }
    }

    void projectionFramesBoundsInsidePadding()
    {
        const QRectF bounds(100.0, 100.0, 5000.0, 400.0);
        const QSizeF minimap(220.0, 150.0);
        const qreal padding = 8.0;
        const auto projection = MinimapProjection::fit(bounds, minimap, padding);

        const QRectF mapped = projection.miniFromWorld(bounds);
        QVERIFY(mapped.left() >= padding - 1e-6);
        QVERIFY(mapped.top() >= padding - 1e-6);
        QVERIFY(mapped.right() <= minimap.width() - padding + 1e-6);
        QVERIFY(mapped.bottom() <= minimap.height() - padding + 1e-6);

        // Uniform scale: the wide board is limited by width and centered vertically.
        QCOMPARE(mapped.center().x(), minimap.width() / 2.0);
        QCOMPARE(mapped.center().y(), minimap.height() / 2.0);
    }

    void projectionSurvivesDegenerateBounds()
    {
        // Empty board.
        const auto empty = MinimapProjection::fit(QRectF(), QSizeF(200, 140), 8.0);
        QVERIFY(empty.scale > 0.0);
        QVERIFY(nearlyEqual(empty.worldFromMini(empty.miniFromWorld(QPointF(3, 4))),
                            QPointF(3, 4)));

        // A single point (zero-size rect) still yields an invertible mapping
        // centered on that point.
        const auto point =
            MinimapProjection::fit(QRectF(500.0, -200.0, 0.0, 0.0),
                                   QSizeF(200, 140), 8.0);
        QVERIFY(point.scale > 0.0);
        const QPointF centreMini = point.miniFromWorld(QPointF(500.0, -200.0));
        QVERIFY(nearlyEqual(centreMini, QPointF(100.0, 70.0), 1e-6));
    }

    void projectionExtremeAspectStaysUniform()
    {
        const QRectF tall(0.0, 0.0, 10.0, 100000.0);
        const auto projection = MinimapProjection::fit(tall, QSizeF(220, 150), 8.0);
        const QRectF mapped = projection.miniFromWorld(tall);
        // One axis fills the padded area; the other keeps the same scale.
        QVERIFY(mapped.height() <= 150.0 - 16.0 + 1e-6);
        QCOMPARE(mapped.width() / tall.width(), mapped.height() / tall.height());
    }

    void cameraCentersWorldPoint()
    {
        CanvasCamera camera;
        camera.zoom = 2.0;
        const QSizeF viewport(1600.0, 1000.0);
        const qreal dpr = 2.0;
        camera.centerOn(QPointF(430.0, -75.0), viewport, dpr);
        QVERIFY(nearlyEqual(camera.screenFromWorld(QPointF(430.0, -75.0), dpr),
                            QPointF(800.0, 500.0)));
        QCOMPARE(camera.zoom, 2.0);
    }

    void cameraFitsRectWithMargin()
    {
        CanvasCamera camera;
        const QRectF world(200.0, 300.0, 4000.0, 1000.0);
        const QSizeF viewport(1280.0, 800.0);
        const qreal dpr = 1.0;
        camera.fitRect(world, viewport, dpr, 0.08);

        // Width-limited: 1280 * 0.84 / 4000.
        QCOMPARE(camera.zoom, 1280.0 * 0.84 / 4000.0);
        QVERIFY(nearlyEqual(camera.screenFromWorld(world.center(), dpr),
                            QPointF(640.0, 400.0)));

        // The whole rect lies inside the viewport.
        const QPointF topLeft = camera.screenFromWorld(world.topLeft(), dpr);
        const QPointF bottomRight = camera.screenFromWorld(world.bottomRight(), dpr);
        QVERIFY(topLeft.x() >= 0.0 && topLeft.y() >= 0.0);
        QVERIFY(bottomRight.x() <= 1280.0 && bottomRight.y() <= 800.0);
    }

    void cameraFitClampsToZoomRange()
    {
        CanvasCamera camera;
        // A tiny rect would need a zoom past the ceiling.
        camera.fitRect(QRectF(0, 0, 1.0, 1.0), QSizeF(1000, 1000), 1.0);
        QCOMPARE(camera.zoom, CanvasCamera::kMaxZoom);
        // A huge rect would need a zoom under the floor.
        camera.fitRect(QRectF(0, 0, 1e7, 1e7), QSizeF(1000, 1000), 1.0);
        QCOMPARE(camera.zoom, CanvasCamera::kMinZoom);
        // A degenerate rect keeps the zoom and recenters.
        camera.zoom = 1.5;
        camera.fitRect(QRectF(50, 50, 0, 0), QSizeF(1000, 1000), 1.0);
        QCOMPARE(camera.zoom, 1.5);
        QVERIFY(nearlyEqual(camera.screenFromWorld(QPointF(50, 50), 1.0),
                            QPointF(500.0, 500.0)));
    }

    void cameraSetsAbsoluteZoomAboutAnchor()
    {
        CanvasCamera camera;
        camera.zoom = 1.0;
        camera.panPixels = QPointF(120.0, -40.0);
        const QPointF anchor(300.0, 200.0);
        const qreal dpr = 2.0;
        const QPointF anchoredWorld = camera.worldFromScreen(anchor, dpr);

        QVERIFY(camera.setZoomAbout(anchor, 2.5, dpr));
        QCOMPARE(camera.zoom, 2.5);
        QVERIFY(nearlyEqual(camera.screenFromWorld(anchoredWorld, dpr), anchor));

        // Clamped and reported unchanged when already at the bound.
        QVERIFY(camera.setZoomAbout(anchor, 100.0, dpr));
        QCOMPARE(camera.zoom, CanvasCamera::kMaxZoom);
        QVERIFY(!camera.setZoomAbout(anchor, 200.0, dpr));
    }
};

QTEST_APPLESS_MAIN(MinimapTest)
#include "tst_minimap.moc"
