#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QSignalSpy>

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/render/PreviewController.h"
#include "cutpilot/render/PreviewItem.h"

using namespace cutpilot;
using core::NodeGraph;
using core::NodeKind;
using render::PreviewController;
using render::PreviewItem;

namespace {

// Promotes the protected key handler so undo can be driven with synthesized
// events on the offscreen platform.
class DrivableLayer : public render::NodeLayerItem {
public:
    using render::NodeLayerItem::keyPressEvent;
};

// The preview's plumbing under test: a board, the controller that owns the
// pins, and the surface receiving the built plans.
struct Rig {
    render::CanvasController camera;
    DrivableLayer layer;
    PreviewItem item;
    PreviewController previews;

    Rig()
    {
        layer.setSize(QSizeF(1600, 1000));
        layer.setController(&camera);
        previews.setLayer(&layer);
        previews.setPreviewItem(&item);
    }

    int addProto(NodeKind kind)
    {
        return layer.graph().addNode(core::compositeNodePrototype(kind));
    }

    void wire(int from, int fromPort, int to, int toPort)
    {
        core::Connection edge;
        edge.fromNodeId = from;
        edge.fromPortIndex = fromPort;
        edge.toNodeId = to;
        edge.toPortIndex = toPort;
        QVERIFY(layer.graph().addConnection(edge) != -1);
    }

    QImage gray(int value)
    {
        QImage image(4, 4, QImage::Format_RGBA8888);
        image.fill(QColor(value, value, value));
        return image;
    }
};

} // namespace

class PreviewWiringTest : public QObject {
    Q_OBJECT

private slots:
    void pinningBuildsThePlanAndSources();
    void pinsAreDecoupledFromSelection();
    void bothBuffersHoldIndependentPins();
    void refreshFollowsParameterChanges();
    void aScrubCommitsAsOneUndoStep();
    void aDeletedPinIsDropped();
    void compareStateLandsInTheItem();
};

void PreviewWiringTest::pinningBuildsThePlanAndSources()
{
    Rig rig;
    const int still = rig.addProto(NodeKind::Still);
    const int blend = rig.addProto(NodeKind::Blend);
    rig.wire(still, 0, blend, 0);
    rig.layer.setNodeMedia(still, rig.gray(120));

    rig.previews.pin(PreviewController::Buffer::A, blend);

    const render::PreviewBufferData &buffer = rig.item.buffer(0);
    QVERIFY(buffer.active);
    QVERIFY(buffer.plan.valid);
    QCOMPARE(buffer.plan.targetNodeId, blend);
    QCOMPARE(buffer.plan.sourceNodeIds, QVector<int>{ still });
    QCOMPARE(buffer.sources.size(), 1);
    QCOMPARE(buffer.sources.first().nodeId, still);
    QVERIFY(!buffer.sources.first().image.isNull());
    QVERIFY(!rig.item.buffer(1).active);
}

void PreviewWiringTest::pinsAreDecoupledFromSelection()
{
    Rig rig;
    const int still = rig.addProto(NodeKind::Still);
    const int transform = rig.addProto(NodeKind::Transform);
    const int other = rig.addProto(NodeKind::Blend);
    rig.wire(still, 0, transform, 0);

    rig.previews.pin(PreviewController::Buffer::A, transform);

    // Selecting something else — the everyday case while tweaking one node
    // and watching another — must not move the pin.
    rig.layer.graph().selectOnly(other);
    rig.previews.refresh();

    QCOMPARE(rig.previews.pinnedNode(PreviewController::Buffer::A), transform);
    QCOMPARE(rig.item.buffer(0).plan.targetNodeId, transform);
}

void PreviewWiringTest::bothBuffersHoldIndependentPins()
{
    Rig rig;
    const int stillA = rig.addProto(NodeKind::Still);
    const int stillB = rig.addProto(NodeKind::Still);

    rig.previews.pin(PreviewController::Buffer::A, stillA);
    rig.previews.pin(PreviewController::Buffer::B, stillB);

    QVERIFY(rig.item.buffer(0).active);
    QVERIFY(rig.item.buffer(1).active);
    QCOMPARE(rig.item.buffer(0).plan.targetNodeId, stillA);
    QCOMPARE(rig.item.buffer(1).plan.targetNodeId, stillB);

    rig.previews.unpin(PreviewController::Buffer::A);
    QVERIFY(!rig.item.buffer(0).active);
    QVERIFY(rig.item.buffer(1).active);
}

void PreviewWiringTest::refreshFollowsParameterChanges()
{
    Rig rig;
    const int still = rig.addProto(NodeKind::Still);
    const int key = rig.addProto(NodeKind::Key);
    rig.wire(still, 0, key, 0);

    rig.previews.pin(PreviewController::Buffer::A, key);
    const QString before = rig.item.buffer(0).plan.passes.last().signature;

    // A scrub writes the parameter and pokes a refresh; the item must see a
    // plan whose tail signature moved, which is what re-renders the pass.
    rig.layer.graph().nodeById(key)->comp.keyTolerance = 0.45;
    rig.previews.refresh();

    const QString after = rig.item.buffer(0).plan.passes.last().signature;
    QVERIFY(before != after);
}

void PreviewWiringTest::aScrubCommitsAsOneUndoStep()
{
    Rig rig;
    const int transform = rig.addProto(NodeKind::Transform);

    // The inspector writes live values while the slider moves, then records
    // the gesture on release; a single undo returns to the pre-scrub state.
    const core::CompositeParams before =
        rig.layer.graph().nodeById(transform)->comp;
    core::CompositeParams live = before;
    for (double degree : { 5.0, 12.0, 33.0 }) {
        live.rotationDeg = degree;
        rig.layer.previewCompositeParams(transform, live);
    }
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp.rotationDeg, 33.0);

    rig.layer.commitCompositeParams(transform, before, live);
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    rig.layer.keyPressEvent(&undo);
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp, before);
}

void PreviewWiringTest::aDeletedPinIsDropped()
{
    Rig rig;
    const int still = rig.addProto(NodeKind::Still);
    const int transform = rig.addProto(NodeKind::Transform);
    rig.wire(still, 0, transform, 0);

    rig.previews.pin(PreviewController::Buffer::A, transform);
    QVERIFY(rig.item.buffer(0).active);

    QSignalSpy pinsSpy(&rig.previews, &PreviewController::pinsChanged);
    rig.layer.graph().removeNode(transform);
    rig.previews.refresh();

    QCOMPARE(rig.previews.pinnedNode(PreviewController::Buffer::A), -1);
    QVERIFY(!rig.item.buffer(0).active);
    QCOMPARE(pinsSpy.count(), 1);
}

void PreviewWiringTest::compareStateLandsInTheItem()
{
    Rig rig;
    rig.item.setCompareMode(PreviewItem::CompareMode::Wipe);
    rig.item.setWipePosition(0.25);
    rig.item.setOverlayOpacity(0.8);
    rig.item.setFitToView(false);

    QCOMPARE(rig.item.compareMode(), PreviewItem::CompareMode::Wipe);
    QCOMPARE(rig.item.wipePosition(), 0.25);
    QCOMPARE(rig.item.overlayOpacity(), 0.8);
    QVERIFY(!rig.item.fitToView());

    // Out-of-range values clamp instead of leaking into the shader.
    rig.item.setWipePosition(1.7);
    QCOMPARE(rig.item.wipePosition(), 1.0);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    PreviewWiringTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_previewwiring.moc"
