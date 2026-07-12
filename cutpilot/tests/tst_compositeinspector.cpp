#include <QtTest/QtTest>

#include <QApplication>
#include <QSlider>

#include "CompositeInspector.h"
#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/render/PreviewController.h"
#include "cutpilot/theme/ThemeTable.h"

using namespace cutpilot;
using core::NodeKind;

namespace {

// Promotes the protected key handler so undo can be driven with synthesized
// events on the offscreen platform.
class DrivableLayer : public render::NodeLayerItem {
public:
    using render::NodeLayerItem::keyPressEvent;
};

// The inspector against the real board: the layer whose command stack takes
// the commits, the preview controller the scrubs poke, and the panel itself.
struct Rig {
    render::CanvasController camera;
    DrivableLayer layer;
    render::PreviewController previews;
    app::CompositeInspector inspector;

    Rig()
        : inspector(theme::ThemeTable(theme::Theme::Dark), &layer, &previews,
                    nullptr)
    {
        layer.setSize(QSizeF(1600, 1000));
        layer.setController(&camera);
        previews.setLayer(&layer);
    }

    int addProto(NodeKind kind)
    {
        return layer.graph().addNode(core::compositeNodePrototype(kind));
    }

    // The panel's slider with this exact range. The panel may rebuild its
    // controls, so callers re-fetch rather than hold pointers across edits.
    QSlider *slider(int min, int max)
    {
        const auto sliders = inspector.findChildren<QSlider *>();
        for (QSlider *s : sliders) {
            if (s->minimum() == min && s->maximum() == max)
                return s;
        }
        return nullptr;
    }

    // One user gesture: grab the handle, move it, let go. The release is
    // what lands the undo step.
    void drag(QSlider *s, int value)
    {
        s->setSliderDown(true);
        s->setValue(value);
        s->setSliderDown(false);
    }

    void undo()
    {
        QKeyEvent z(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        layer.keyPressEvent(&z);
    }
};

} // namespace

class CompositeInspectorTest : public QObject {
    Q_OBJECT

private slots:
    void aGestureLandsAsOneUndoableStep();
    void undoSurvivesTheNextGesture();
    void controlsFollowAnUndoneValue();
};

void CompositeInspectorTest::aGestureLandsAsOneUndoableStep()
{
    Rig rig;
    const int transform = rig.addProto(NodeKind::Transform);
    rig.inspector.openFor(transform);

    QSlider *rotation = rig.slider(-180, 180);
    QVERIFY(rotation);
    rig.drag(rotation, 33);
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp.rotationDeg, 33.0);

    rig.undo();
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp.rotationDeg, 0.0);
}

void CompositeInspectorTest::undoSurvivesTheNextGesture()
{
    Rig rig;
    const int transform = rig.addProto(NodeKind::Transform);
    rig.inspector.openFor(transform);

    QSlider *rotation = rig.slider(-180, 180);
    QVERIFY(rotation);
    rig.drag(rotation, 33);
    rig.undo();
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp.rotationDeg, 0.0);

    // The panel stayed open across the undo. Touching a different control
    // must not smuggle the undone rotation back onto the graph.
    QSlider *scale = rig.slider(10, 400);
    QVERIFY(scale);
    rig.drag(scale, 150);
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp.scale, 1.5);
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp.rotationDeg, 0.0);

    // And the scale gesture's own undo entry must snapshot what was really
    // on the graph before it — not the panel's stale pre-undo state.
    rig.undo();
    QCOMPARE(rig.layer.graph().nodeById(transform)->comp,
             core::CompositeParams());
}

void CompositeInspectorTest::controlsFollowAnUndoneValue()
{
    Rig rig;
    const int transform = rig.addProto(NodeKind::Transform);
    rig.inspector.openFor(transform);

    QSlider *rotation = rig.slider(-180, 180);
    QVERIFY(rotation);
    rig.drag(rotation, 33);
    rig.undo();

    rotation = rig.slider(-180, 180);
    QVERIFY(rotation);
    QCOMPARE(rotation->value(), 0);

    // Layout-added children are shown through a queued event; after the
    // loop spins, the rebuilt controls must be visible in the open panel.
    QCoreApplication::processEvents();
    QVERIFY(rotation->isVisible());
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    CompositeInspectorTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_compositeinspector.moc"
