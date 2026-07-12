#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QSignalSpy>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"

using namespace cutpilot;

namespace {

// Promotes the protected event handlers so the wiring gestures can be driven with
// synthesized events on the offscreen platform, without a window server.
class DrivableLayer : public render::NodeLayerItem {
public:
    using render::NodeLayerItem::keyPressEvent;
    using render::NodeLayerItem::mouseDoubleClickEvent;
    using render::NodeLayerItem::mouseMoveEvent;
    using render::NodeLayerItem::mousePressEvent;
    using render::NodeLayerItem::mouseReleaseEvent;
};

void press(DrivableLayer &layer, const QPointF &pos)
{
    QMouseEvent event(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    layer.mousePressEvent(&event);
}

void move(DrivableLayer &layer, const QPointF &pos)
{
    QMouseEvent event(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::LeftButton,
                      Qt::NoModifier);
    layer.mouseMoveEvent(&event);
}

void release(DrivableLayer &layer, const QPointF &pos)
{
    QMouseEvent event(QEvent::MouseButtonRelease, pos, pos, Qt::LeftButton,
                      Qt::NoButton, Qt::NoModifier);
    layer.mouseReleaseEvent(&event);
}

void drag(DrivableLayer &layer, const QPointF &from, const QPointF &to)
{
    press(layer, from);
    move(layer, (from + to) / 2.0);
    move(layer, to);
    release(layer, to);
}

void key(DrivableLayer &layer, Qt::Key k, Qt::KeyboardModifiers mods)
{
    QKeyEvent event(QEvent::KeyPress, k, mods);
    layer.keyPressEvent(&event);
}

void rightPress(DrivableLayer &layer, const QPointF &pos)
{
    QMouseEvent event(QEvent::MouseButtonPress, pos, pos, Qt::RightButton,
                      Qt::RightButton, Qt::NoModifier);
    layer.mousePressEvent(&event);
}

void doubleClick(DrivableLayer &layer, const QPointF &pos)
{
    QMouseEvent event(QEvent::MouseButtonDblClick, pos, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    layer.mouseDoubleClickEvent(&event);
}

// Button-explicit variants for gestures that mix mouse buttons.
void pressButton(DrivableLayer &layer, const QPointF &pos, Qt::MouseButton button,
                 Qt::MouseButtons alreadyHeld)
{
    QMouseEvent event(QEvent::MouseButtonPress, pos, pos, button,
                      alreadyHeld | button, Qt::NoModifier);
    layer.mousePressEvent(&event);
}

void releaseButton(DrivableLayer &layer, const QPointF &pos, Qt::MouseButton button,
                   Qt::MouseButtons stillHeld)
{
    QMouseEvent event(QEvent::MouseButtonRelease, pos, pos, button, stillHeld,
                      Qt::NoModifier);
    layer.mouseReleaseEvent(&event);
}

void moveButtons(DrivableLayer &layer, const QPointF &pos, Qt::MouseButtons held)
{
    QMouseEvent event(QEvent::MouseMove, pos, pos, Qt::NoButton, held,
                      Qt::NoModifier);
    layer.mouseMoveEvent(&event);
}

// True when every connection's endpoints resolve to live nodes.
bool allEndpointsExist(const cutpilot::core::NodeGraph &graph)
{
    for (const cutpilot::core::Connection &c : graph.connections()) {
        if (!graph.nodeById(c.fromNodeId) || !graph.nodeById(c.toNodeId))
            return false;
    }
    return true;
}

// With the default camera (zoom 1, no pan, ratio 1 offscreen), item coordinates
// equal world coordinates, so the default node's port positions are exact. The
// default node is 280x200 centred on the given point, with an image input at
// fraction 0.3, a text input at 0.55, a control input at 0.8, and an image output
// at 0.5.
struct Board {
    render::CanvasController controller;
    DrivableLayer layer;

    Board()
    {
        layer.setSize(QSizeF(1600, 1000));
        layer.setController(&controller);
    }

    int addDefaultNode(const QPointF &centre)
    {
        layer.addNodeAtCursor(centre);
        return layer.graph().nodes().last().id;
    }
};

QPointF outputPort(const QPointF &centre)
{
    return centre + QPointF(140, 0);
}

QPointF imageInputPort(const QPointF &centre)
{
    return centre + QPointF(-140, -40); // fraction 0.3 of the 200-world height
}

QPointF textInputPort(const QPointF &centre)
{
    return centre + QPointF(-140, 10); // fraction 0.55
}

QPointF controlInputPort(const QPointF &centre)
{
    return centre + QPointF(-140, 60); // fraction 0.8
}

} // namespace

class TstNodeLayerWiring : public QObject {
    Q_OBJECT

private slots:
    void dragOutputToCompatibleInputConnects();
    void incompatibleDropIsRefused();
    void connectGestureUndoesAndRedoes();
    void movingAConnectedNodeKeepsTheEdgeAndMovesItsPort();
    void draggingOccupiedInputOffToEmptyCanvasDisconnects();
    void reroutingToAnotherInputIsOneUndoStep();
    void emptyCanvasDropRaisesTypeFilteredPalette();
    void deleteDuringConnectDragCannotOrphanAConnection();
    void undoDuringConnectDragCannotOrphanAConnection();
    void flickReleaseWithoutTrailingMoveStillConnects();
    void leftReleaseDuringMiddlePanEndsTheWireNotThePan();
    void rightPressOnAGenerationNodeRaisesItsRunMenu();
    void controlDropOffersTheCostGateAndItsLimitIsEditable();
};

void TstNodeLayerWiring::dragOutputToCompatibleInputConnects()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const int aId = board.addDefaultNode(a);
    const int bId = board.addDefaultNode(b);

    drag(board.layer, outputPort(a), imageInputPort(b));

    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.connections().size(), 1);
    const core::Connection wire = graph.connections().first();
    QCOMPARE(wire.fromNodeId, aId);
    QCOMPARE(wire.fromPortIndex, 3); // the image output
    QCOMPARE(wire.toNodeId, bId);
    QCOMPARE(wire.toPortIndex, 0); // the image input
}

void TstNodeLayerWiring::incompatibleDropIsRefused()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    board.addDefaultNode(a);
    board.addDefaultNode(b);

    // An image output refuses a text input, a control input, and another output.
    drag(board.layer, outputPort(a), textInputPort(b));
    drag(board.layer, outputPort(a), controlInputPort(b));
    drag(board.layer, outputPort(a), outputPort(b));

    QCOMPARE(board.layer.graph().connections().size(), 0);
}

void TstNodeLayerWiring::connectGestureUndoesAndRedoes()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    board.addDefaultNode(a);
    board.addDefaultNode(b);

    drag(board.layer, outputPort(a), imageInputPort(b));
    QCOMPARE(board.layer.graph().connections().size(), 1);
    const int wireId = board.layer.graph().connections().first().id;

    key(board.layer, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(board.layer.graph().connections().size(), 0);

    key(board.layer, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(board.layer.graph().connections().size(), 1);
    QCOMPARE(board.layer.graph().connections().first().id, wireId);
}

void TstNodeLayerWiring::movingAConnectedNodeKeepsTheEdgeAndMovesItsPort()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const int bId = board.addDefaultNode(b);
    board.addDefaultNode(a); // added second so the drag below grabs a clear body spot
    drag(board.layer, outputPort(a), imageInputPort(b));
    QCOMPARE(board.layer.graph().connections().size(), 1);

    // Drag the target node by its body. The edge must survive, and the port the
    // connector re-derives from must land exactly where the node moved.
    const QPointF grip = b + QPointF(0, 30); // clear of every port's grab radius
    drag(board.layer, grip, grip + QPointF(150, -120));

    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().toNodeId, bId);
    const core::Node *moved = graph.nodeById(bId);
    QCOMPARE(moved->portWorldPosition(0),
             imageInputPort(b) + QPointF(150, -120));
}

void TstNodeLayerWiring::draggingOccupiedInputOffToEmptyCanvasDisconnects()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    board.addDefaultNode(a);
    board.addDefaultNode(b);
    drag(board.layer, outputPort(a), imageInputPort(b));
    QCOMPARE(board.layer.graph().connections().size(), 1);

    // Lift the edge off the occupied input and release over empty canvas.
    drag(board.layer, imageInputPort(b), QPointF(600, 850));
    QCOMPARE(board.layer.graph().connections().size(), 0);

    // The disconnect is one undoable step.
    key(board.layer, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(board.layer.graph().connections().size(), 1);
}

void TstNodeLayerWiring::reroutingToAnotherInputIsOneUndoStep()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const QPointF c(900, 800);
    const int aId = board.addDefaultNode(a);
    const int bId = board.addDefaultNode(b);
    const int cId = board.addDefaultNode(c);

    drag(board.layer, outputPort(a), imageInputPort(b));

    // Grab the edge off b's input and drop it on c's input.
    drag(board.layer, imageInputPort(b), imageInputPort(c));
    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().fromNodeId, aId);
    QCOMPARE(graph.connections().first().toNodeId, cId);

    // One undo restores the original wiring.
    key(board.layer, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().toNodeId, bId);
}

void TstNodeLayerWiring::emptyCanvasDropRaisesTypeFilteredPalette()
{
    Board board;
    const QPointF a(300, 300);
    const int aId = board.addDefaultNode(a);

    QSignalSpy spy(&board.layer, &render::NodeLayerItem::paletteRequested);
    const QPointF drop(900, 700);
    drag(board.layer, outputPort(a), drop);
    QCOMPARE(spy.count(), 1);

    // The offers accept an image: generators and image processors qualify, while
    // source-only, text-only, and control-only nodes are filtered out.
    const QStringList titles = board.layer.paletteEntryTitles();
    QVERIFY(titles.contains(QStringLiteral("Generate Image")));
    QVERIFY(titles.contains(QStringLiteral("Upscale Image")));
    QVERIFY(!titles.contains(QStringLiteral("Prompt")));
    QVERIFY(!titles.contains(QStringLiteral("Generate Voice")));
    QVERIFY(!titles.contains(QStringLiteral("Batch Count")));
    QVERIFY(!titles.contains(QStringLiteral("Run Gate")));

    // Placing an entry adds the node wired to the anchor, as one undo step.
    board.layer.placePaletteEntry(titles.indexOf(QStringLiteral("Upscale Image")));
    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.nodes().size(), 2);
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().fromNodeId, aId);

    key(board.layer, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(graph.nodes().size(), 1);
    QCOMPARE(graph.connections().size(), 0);

    // Cancelling instead of placing leaves the board untouched.
    drag(board.layer, outputPort(a), drop);
    board.layer.cancelPalette();
    QCOMPARE(graph.nodes().size(), 1);
    QCOMPARE(graph.connections().size(), 0);
}

void TstNodeLayerWiring::deleteDuringConnectDragCannotOrphanAConnection()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const int aId = board.addDefaultNode(a);
    const int bId = board.addDefaultNode(b);

    // Select the source node by its body, clear of every port's grab radius.
    const QPointF grip = a + QPointF(0, 30);
    press(board.layer, grip);
    release(board.layer, grip);
    QVERIFY(board.layer.graph().nodeById(aId)->selected);

    // Start wiring from the selected node's output, then delete it mid-drag.
    press(board.layer, outputPort(a));
    move(board.layer, QPointF(600, 380));
    key(board.layer, Qt::Key_Delete, Qt::NoModifier);
    move(board.layer, imageInputPort(b));
    release(board.layer, imageInputPort(b));

    const core::NodeGraph &graph = board.layer.graph();
    QVERIFY(!graph.nodeById(aId)); // the delete landed
    QVERIFY(graph.nodeById(bId));
    QCOMPARE(graph.connections().size(), 0); // no edge to a dead node
    QVERIFY(allEndpointsExist(graph));
}

void TstNodeLayerWiring::undoDuringConnectDragCannotOrphanAConnection()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const int bId = board.addDefaultNode(b);
    const int aId = board.addDefaultNode(a); // added last, so undo removes it

    // Start wiring from the newest node's output, then undo its add mid-drag.
    press(board.layer, outputPort(a));
    move(board.layer, QPointF(600, 380));
    key(board.layer, Qt::Key_Z, Qt::ControlModifier);
    move(board.layer, imageInputPort(b));
    release(board.layer, imageInputPort(b));

    const core::NodeGraph &graph = board.layer.graph();
    QVERIFY(!graph.nodeById(aId)); // the undo landed
    QVERIFY(graph.nodeById(bId));
    QCOMPARE(graph.connections().size(), 0); // no edge to a dead node
    QVERIFY(allEndpointsExist(graph));
}

void TstNodeLayerWiring::flickReleaseWithoutTrailingMoveStillConnects()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const int aId = board.addDefaultNode(a);
    const int bId = board.addDefaultNode(b);

    // A fast flick: the pointer's last move lands mid-canvas and the release
    // arrives straight at the target port with no move event there.
    press(board.layer, outputPort(a));
    move(board.layer, QPointF(600, 380));
    release(board.layer, imageInputPort(b));

    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.connections().size(), 1);
    const core::Connection wire = graph.connections().first();
    QCOMPARE(wire.fromNodeId, aId);
    QCOMPARE(wire.fromPortIndex, 3);
    QCOMPARE(wire.toNodeId, bId);
    QCOMPARE(wire.toPortIndex, 0);
}

void TstNodeLayerWiring::leftReleaseDuringMiddlePanEndsTheWireNotThePan()
{
    Board board;
    const QPointF a(300, 300);
    const QPointF b(900, 500);
    const int aId = board.addDefaultNode(a);
    const int bId = board.addDefaultNode(b);

    // A left-button wire-drag and a middle-button pan run at the same time.
    press(board.layer, outputPort(a));
    moveButtons(board.layer, QPointF(600, 380), Qt::LeftButton);
    pressButton(board.layer, QPointF(600, 380), Qt::MiddleButton, Qt::LeftButton);

    // Releasing the left button ends the wire gesture: the wire commits at the
    // release point and must not be misread as the end of the pan.
    releaseButton(board.layer, imageInputPort(b), Qt::LeftButton, Qt::MiddleButton);
    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.connections().size(), 1);
    QCOMPARE(graph.connections().first().fromNodeId, aId);
    QCOMPARE(graph.connections().first().toNodeId, bId);

    // The pan is still live: moving with the middle button held pans the camera.
    const QPointF panBefore = board.controller.panPixels();
    moveButtons(board.layer, imageInputPort(b) + QPointF(20, 20), Qt::MiddleButton);
    QVERIFY(board.controller.panPixels() != panBefore);

    // Releasing the middle button ends the pan; a further move pans no more,
    // and the finished wire gesture leaves the model alone.
    releaseButton(board.layer, imageInputPort(b) + QPointF(20, 20),
                  Qt::MiddleButton, Qt::NoButton);
    const QPointF panAfter = board.controller.panPixels();
    moveButtons(board.layer, imageInputPort(b) + QPointF(60, 60), Qt::NoButton);
    QCOMPARE(board.controller.panPixels(), panAfter);
    QCOMPARE(graph.connections().size(), 1);
}

void TstNodeLayerWiring::rightPressOnAGenerationNodeRaisesItsRunMenu()
{
    Board board;
    const QPointF centre(500, 400);
    const int id = board.addDefaultNode(centre);

    QSignalSpy menuSpy(&board.layer, &render::NodeLayerItem::nodeMenuRequested);
    rightPress(board.layer, centre);
    QCOMPARE(menuSpy.count(), 1);
    QCOMPARE(menuSpy.first().first().toInt(), id);
    QVERIFY(board.layer.graph().nodeById(id)->selected);

    // Empty canvas offers no run menu.
    rightPress(board.layer, QPointF(1200, 900));
    QCOMPARE(menuSpy.count(), 1);
}

void TstNodeLayerWiring::controlDropOffersTheCostGateAndItsLimitIsEditable()
{
    Board board;
    const QPointF centre(900, 500);
    const int nodeId = board.addDefaultNode(centre);

    // Dragging a fresh wire off the run control input to empty canvas offers
    // only nodes with a control output — the cost gate.
    QSignalSpy paletteSpy(&board.layer, &render::NodeLayerItem::paletteRequested);
    const QPointF drop(300, 500);
    drag(board.layer, controlInputPort(centre), drop);
    QCOMPARE(paletteSpy.count(), 1);
    const QStringList titles = board.layer.paletteEntryTitles();
    QVERIFY(titles.contains(QStringLiteral("Cost Gate")));
    QVERIFY(!titles.contains(QStringLiteral("Upscale Image")));

    board.layer.placePaletteEntry(titles.indexOf(QStringLiteral("Cost Gate")));
    const core::NodeGraph &graph = board.layer.graph();
    QCOMPARE(graph.nodes().size(), 2);
    QCOMPARE(graph.connections().size(), 1);
    const core::Node &gate = graph.nodes().last();
    QCOMPARE(gate.kind, core::NodeKind::CostGate);
    QCOMPARE(graph.connections().first().fromNodeId, gate.id);
    QCOMPARE(graph.connections().first().toNodeId, nodeId);

    // Double-clicking the gate asks the chrome for its limit editor.
    QSignalSpy limitSpy(&board.layer, &render::NodeLayerItem::gateLimitEditRequested);
    doubleClick(board.layer, gate.worldRect().center());
    QCOMPARE(limitSpy.count(), 1);
    QCOMPARE(limitSpy.first().first().toInt(), gate.id);

    // The limit is a real undoable parameter.
    const double initial = gate.gateLimitUsd;
    board.layer.setGateLimit(gate.id, 0.25);
    QCOMPARE(graph.nodeById(gate.id)->gateLimitUsd, 0.25);
    key(board.layer, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(graph.nodeById(gate.id)->gateLimitUsd, initial);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    TstNodeLayerWiring testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_nodelayerwiring.moc"
