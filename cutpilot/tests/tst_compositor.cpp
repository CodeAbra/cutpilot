#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QImage>

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/CompositePlan.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CompositorEngine.h"

#include <cmath>
#include <memory>

using namespace cutpilot;
using core::BlendMode;
using core::NodeGraph;
using core::NodeKind;

namespace {

QImage uniformImage(int w, int h, const QColor &color)
{
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

int addProto(NodeGraph &graph, NodeKind kind)
{
    return graph.addNode(core::compositeNodePrototype(kind));
}

void wire(NodeGraph &graph, int from, int fromPort, int to, int toPort)
{
    core::Connection edge;
    edge.fromNodeId = from;
    edge.fromPortIndex = fromPort;
    edge.toNodeId = to;
    edge.toPortIndex = toPort;
    QVERIFY(graph.addConnection(edge) != -1);
}

// Reference math mirroring the fragment shaders in double precision, over
// the same 8-bit inputs, so the GPU result must land within rounding of it.
struct Rgba {
    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
};

Rgba fromColor(const QColor &c)
{
    return { c.red() / 255.0, c.green() / 255.0, c.blue() / 255.0,
             c.alpha() / 255.0 };
}

double blendChannel(BlendMode mode, double b, double s)
{
    switch (mode) {
    case BlendMode::Multiply:
        return b * s;
    case BlendMode::Screen:
        return b + s - b * s;
    case BlendMode::Overlay:
        return b < 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    case BlendMode::Add:
        return std::min(b + s, 1.0);
    case BlendMode::Normal:
        break;
    }
    return s;
}

Rgba blendReference(BlendMode mode, double opacity, const Rgba &base,
                    const Rgba &over)
{
    const double as = std::clamp(over.a * opacity, 0.0, 1.0);
    const double ab = base.a;
    const auto channel = [&](double b, double s) {
        const double mixed = s + (blendChannel(mode, b, s) - s) * ab;
        const double ao = as + ab * (1.0 - as);
        if (ao <= 0.0)
            return 0.0;
        return (as * mixed + ab * b * (1.0 - as)) / ao;
    };
    Rgba out;
    out.r = channel(base.r, over.r);
    out.g = channel(base.g, over.g);
    out.b = channel(base.b, over.b);
    out.a = as + ab * (1.0 - as);
    return out;
}

double smoothstepRef(double edge0, double edge1, double x)
{
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool pixelNear(const QImage &image, int x, int y, const Rgba &expected,
               int tolerance, QByteArray *message)
{
    const QColor actual = image.pixelColor(x, y);
    const int er = qRound(expected.r * 255.0);
    const int eg = qRound(expected.g * 255.0);
    const int eb = qRound(expected.b * 255.0);
    const int ea = qRound(expected.a * 255.0);
    const bool ok = qAbs(actual.red() - er) <= tolerance
        && qAbs(actual.green() - eg) <= tolerance
        && qAbs(actual.blue() - eb) <= tolerance
        && qAbs(actual.alpha() - ea) <= tolerance;
    if (!ok && message) {
        *message = QStringLiteral("pixel (%1,%2): got %3,%4,%5,%6 expected "
                                  "%7,%8,%9,%10")
                       .arg(x)
                       .arg(y)
                       .arg(actual.red())
                       .arg(actual.green())
                       .arg(actual.blue())
                       .arg(actual.alpha())
                       .arg(er)
                       .arg(eg)
                       .arg(eb)
                       .arg(ea)
                       .toLatin1();
    }
    return ok;
}

#define CHECK_PIXEL(image, x, y, expected, tolerance)                          \
    do {                                                                       \
        QByteArray pixelMessage;                                               \
        QVERIFY2(pixelNear(image, x, y, expected, tolerance, &pixelMessage),   \
                 pixelMessage.constData());                                    \
    } while (false)

} // namespace

class CompositorTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void passthroughKeepsPixelsAndOrientation();
    void blendModesMatchTheReference();
    void blendWithoutABaseIsTheOverLayer();
    void unwiredBlendIsTransparent();
    void keySeparatesByDistance();
    void maskAppliesTheMatteAndInverts();
    void matteConsumedAsImageReadsGray();
    void transformTranslatesRotatesAndScales();
    void cacheServesUnchangedPassesAndScrubOnlyTheTail();

};

// Node ids restart per test graph, and the engine caches by node id, so each
// test runs its own engine exactly as each open board would.
#define REQUIRE_ENGINE(engine)                                                 \
    render::CompositorEngine engine;                                           \
    QVERIFY(engine.adoptHeadlessDevice())

void CompositorTest::initTestCase()
{
    render::CompositorEngine probe;
    if (!probe.adoptHeadlessDevice())
        QSKIP("No headless GPU device on this machine; the compositor's "
              "pixel math cannot be verified here");
}

void CompositorTest::passthroughKeepsPixelsAndOrientation()
{
    REQUIRE_ENGINE(engine);
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int mask = addProto(graph, NodeKind::Mask);
    wire(graph, still, 0, mask, 0); // no mask wired: a pass-through

    QImage image = uniformImage(8, 8, QColor(90, 90, 90));
    image.setPixelColor(0, 0, QColor(255, 0, 0));
    image.setPixelColor(7, 0, QColor(0, 255, 0));
    image.setPixelColor(0, 7, QColor(0, 0, 255));
    image.setPixelColor(7, 7, QColor(255, 255, 255));
    engine.setSource(still, image, 1);

    const QImage out = engine.evaluateToImage(buildCompositePlan(graph, mask));
    QCOMPARE(out.size(), QSize(8, 8));
    CHECK_PIXEL(out, 0, 0, fromColor(QColor(255, 0, 0)), 0);
    CHECK_PIXEL(out, 7, 0, fromColor(QColor(0, 255, 0)), 0);
    CHECK_PIXEL(out, 0, 7, fromColor(QColor(0, 0, 255)), 0);
    CHECK_PIXEL(out, 7, 7, fromColor(QColor(255, 255, 255)), 0);
    CHECK_PIXEL(out, 3, 4, fromColor(QColor(90, 90, 90)), 0);
}

void CompositorTest::blendModesMatchTheReference()
{
    REQUIRE_ENGINE(engine);
    const QColor baseColor(128, 64, 191, 255);
    const QColor overColor(64, 160, 32, 204);
    const double opacity = 0.6;

    for (int mode = 0; mode < core::blendModeCount(); ++mode) {
        NodeGraph graph;
        const int base = addProto(graph, NodeKind::Still);
        const int over = addProto(graph, NodeKind::Still);
        const int blend = addProto(graph, NodeKind::Blend);
        wire(graph, base, 0, blend, 0);
        wire(graph, over, 0, blend, 1);
        graph.nodeById(blend)->comp.blendMode = BlendMode(mode);
        graph.nodeById(blend)->comp.opacity = opacity;

        engine.setSource(base, uniformImage(4, 4, baseColor), 1);
        engine.setSource(over, uniformImage(4, 4, overColor), 1);

        const QImage out =
            engine.evaluateToImage(buildCompositePlan(graph, blend));
        QVERIFY2(!out.isNull(),
                 qPrintable(QStringLiteral("no output for %1")
                                .arg(core::blendModeLabel(BlendMode(mode)))));
        const Rgba expected = blendReference(BlendMode(mode), opacity,
                                             fromColor(baseColor),
                                             fromColor(overColor));
        CHECK_PIXEL(out, 1, 2, expected, 2);
    }
}

void CompositorTest::blendWithoutABaseIsTheOverLayer()
{
    REQUIRE_ENGINE(engine);
    NodeGraph graph;
    const int over = addProto(graph, NodeKind::Still);
    const int blend = addProto(graph, NodeKind::Blend);
    wire(graph, over, 0, blend, 1);
    graph.nodeById(blend)->comp.opacity = 0.5;

    engine.setSource(over, uniformImage(4, 4, QColor(200, 40, 120, 255)), 1);
    const QImage out = engine.evaluateToImage(buildCompositePlan(graph, blend));

    Rgba expected = fromColor(QColor(200, 40, 120, 255));
    expected.a = 0.5;
    CHECK_PIXEL(out, 2, 2, expected, 2);
}

void CompositorTest::unwiredBlendIsTransparent()
{
    REQUIRE_ENGINE(engine);
    NodeGraph graph;
    const int blend = addProto(graph, NodeKind::Blend);
    const QImage out = engine.evaluateToImage(buildCompositePlan(graph, blend));
    QVERIFY(!out.isNull());
    QCOMPARE(out.pixelColor(0, 0).alpha(), 0);
}

void CompositorTest::keySeparatesByDistance()
{
    REQUIRE_ENGINE(engine);
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    wire(graph, still, 0, key, 0);

    core::Node *keyNode = graph.nodeById(key);
    keyNode->comp.keyColor = QColor(0, 0, 0);
    keyNode->comp.keyTolerance = 0.2;
    keyNode->comp.keySoftness = 0.2;

    // A gray level's distance to black is its own value, so the keep factor
    // is the smoothstep of the gray: 0 inside tolerance, 1 beyond the band.
    QImage image = uniformImage(6, 6, QColor(10, 10, 10)); // inside: removed
    image.setPixelColor(1, 1, QColor(230, 230, 230));      // far: kept
    image.setPixelColor(2, 2, QColor(76, 76, 76));         // inside the band
    engine.setSource(still, image, 1);

    const QImage out = engine.evaluateToImage(buildCompositePlan(graph, key));

    Rgba removed = fromColor(QColor(10, 10, 10, 255));
    removed.a = 0.0;
    CHECK_PIXEL(out, 4, 4, removed, 1);

    CHECK_PIXEL(out, 1, 1, fromColor(QColor(230, 230, 230, 255)), 1);

    Rgba banded = fromColor(QColor(76, 76, 76, 255));
    banded.a = smoothstepRef(0.2, 0.4, 76.0 / 255.0);
    CHECK_PIXEL(out, 2, 2, banded, 2);
}

void CompositorTest::maskAppliesTheMatteAndInverts()
{
    REQUIRE_ENGINE(engine);
    // The key's matte output carries its alpha; the mask node multiplies the
    // image's alpha by that signal, or by its inverse.
    NodeGraph graph;
    const int keySource = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int imageSource = addProto(graph, NodeKind::Still);
    const int mask = addProto(graph, NodeKind::Mask);
    wire(graph, keySource, 0, key, 0);
    wire(graph, imageSource, 0, mask, 0);
    wire(graph, key, 2, mask, 1); // the matte output feeds the mask input

    core::Node *keyNode = graph.nodeById(key);
    keyNode->comp.keyColor = QColor(0, 0, 0);
    keyNode->comp.keyTolerance = 0.2;
    keyNode->comp.keySoftness = 0.2;

    // The key source is black: fully keyed, matte 0 everywhere.
    engine.setSource(keySource, uniformImage(6, 6, QColor(0, 0, 0)), 1);
    engine.setSource(imageSource, uniformImage(6, 6, QColor(200, 100, 50)), 1);

    QImage out = engine.evaluateToImage(buildCompositePlan(graph, mask));
    Rgba masked = fromColor(QColor(200, 100, 50, 255));
    masked.a = 0.0;
    CHECK_PIXEL(out, 3, 3, masked, 1);

    // Inverting the mask restores full opacity.
    graph.nodeById(mask)->comp.invertMask = true;
    out = engine.evaluateToImage(buildCompositePlan(graph, mask));
    CHECK_PIXEL(out, 3, 3, fromColor(QColor(200, 100, 50, 255)), 1);
}

void CompositorTest::matteConsumedAsImageReadsGray()
{
    REQUIRE_ENGINE(engine);
    // A matte wired into an image input (the permitted mask-to-image
    // conversion) reads as an opaque gray of its alpha.
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int transform = addProto(graph, NodeKind::Transform);
    wire(graph, still, 0, key, 0);
    wire(graph, key, 2, transform, 0);

    core::Node *keyNode = graph.nodeById(key);
    keyNode->comp.keyColor = QColor(0, 0, 0);
    keyNode->comp.keyTolerance = 0.2;
    keyNode->comp.keySoftness = 0.2;

    // Far from the key color: matte 1 → white; the matte of a keyed-out
    // pixel would read black.
    engine.setSource(still, uniformImage(6, 6, QColor(230, 230, 230)), 1);
    const QImage out =
        engine.evaluateToImage(buildCompositePlan(graph, transform));
    CHECK_PIXEL(out, 3, 3, fromColor(QColor(255, 255, 255, 255)), 1);
}

void CompositorTest::transformTranslatesRotatesAndScales()
{
    REQUIRE_ENGINE(engine);
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int transform = addProto(graph, NodeKind::Transform);
    wire(graph, still, 0, transform, 0);

    QImage image = uniformImage(8, 8, QColor(40, 40, 40));
    image.setPixelColor(1, 1, QColor(255, 0, 0));
    engine.setSource(still, image, 1);

    // Translate by exactly two texels: the marker moves with it, the vacated
    // left band maps from outside the image and goes transparent.
    graph.nodeById(transform)->comp.translateX = 2.0 / 8.0;
    QImage out = engine.evaluateToImage(buildCompositePlan(graph, transform));
    CHECK_PIXEL(out, 3, 1, fromColor(QColor(255, 0, 0)), 1);
    QCOMPARE(out.pixelColor(1, 1).alpha(), 0);
    CHECK_PIXEL(out, 5, 4, fromColor(QColor(40, 40, 40)), 1);

    // A half turn lands the marker at the mirrored texel.
    graph.nodeById(transform)->comp = core::CompositeParams();
    graph.nodeById(transform)->comp.rotationDeg = 180.0;
    out = engine.evaluateToImage(buildCompositePlan(graph, transform));
    CHECK_PIXEL(out, 6, 6, fromColor(QColor(255, 0, 0)), 1);

    // Scaling down leaves the outside transparent and the center filled.
    graph.nodeById(transform)->comp = core::CompositeParams();
    graph.nodeById(transform)->comp.scale = 0.5;
    out = engine.evaluateToImage(buildCompositePlan(graph, transform));
    QCOMPARE(out.pixelColor(0, 0).alpha(), 0);
    CHECK_PIXEL(out, 4, 4, fromColor(QColor(40, 40, 40)), 1);
}

void CompositorTest::cacheServesUnchangedPassesAndScrubOnlyTheTail()
{
    REQUIRE_ENGINE(engine);
    NodeGraph graph;
    const int stillA = addProto(graph, NodeKind::Still);
    const int stillB = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int blend = addProto(graph, NodeKind::Blend);
    wire(graph, stillA, 0, key, 0);
    wire(graph, stillB, 0, blend, 0);
    wire(graph, key, 1, blend, 1);

    engine.setSource(stillA, uniformImage(8, 8, QColor(0, 200, 0)), 1);
    engine.setSource(stillB, uniformImage(8, 8, QColor(50, 50, 200)), 1);

    // First evaluation renders both passes; an unchanged graph renders none.
    QVERIFY(!engine.evaluateToImage(buildCompositePlan(graph, blend)).isNull());
    QCOMPARE(engine.lastPassCount(), 2);
    QVERIFY(!engine.evaluateToImage(buildCompositePlan(graph, blend)).isNull());
    QCOMPARE(engine.lastPassCount(), 0);

    // Scrubbing the tail re-renders the tail alone.
    graph.nodeById(blend)->comp.opacity = 0.7;
    QVERIFY(!engine.evaluateToImage(buildCompositePlan(graph, blend)).isNull());
    QCOMPARE(engine.lastPassCount(), 1);

    // Scrubbing upstream re-renders it and everything after it.
    graph.nodeById(key)->comp.keyTolerance = 0.4;
    QVERIFY(!engine.evaluateToImage(buildCompositePlan(graph, blend)).isNull());
    QCOMPARE(engine.lastPassCount(), 2);

    // New source pixels invalidate the plan's passes.
    engine.setSource(stillB, uniformImage(8, 8, QColor(90, 10, 10)), 2);
    QVERIFY(!engine.evaluateToImage(buildCompositePlan(graph, blend)).isNull());
    QCOMPARE(engine.lastPassCount(), 2);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    CompositorTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_compositor.moc"
