#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QSignalSpy>

#include "cutpilot/core/ConnectorPath.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/MinimapItem.h"
#include "cutpilot/render/NodeLayerItem.h"

using namespace cutpilot;

namespace {

// Promotes the protected event handlers so the tool gestures can be driven with
// synthesized events on the offscreen platform, without a window server.
class DrivableLayer : public render::NodeLayerItem {
public:
    using render::NodeLayerItem::keyPressEvent;
    using render::NodeLayerItem::mouseDoubleClickEvent;
    using render::NodeLayerItem::mouseMoveEvent;
    using render::NodeLayerItem::mousePressEvent;
    using render::NodeLayerItem::mouseReleaseEvent;
};

class DrivableMinimap : public render::MinimapItem {
public:
    using render::MinimapItem::mouseMoveEvent;
    using render::MinimapItem::mousePressEvent;
    using render::MinimapItem::mouseReleaseEvent;
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

void doubleClick(DrivableLayer &layer, const QPointF &pos)
{
    QMouseEvent event(QEvent::MouseButtonDblClick, pos, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    layer.mouseDoubleClickEvent(&event);
}

void key(DrivableLayer &layer, Qt::Key k, Qt::KeyboardModifiers mods)
{
    QKeyEvent event(QEvent::KeyPress, k, mods);
    layer.keyPressEvent(&event);
}

// With the default camera (zoom 1, no pan, ratio 1 offscreen), item coordinates
// equal world coordinates. The default node is 280x200 centred on the given
// point with its image input at fraction 0.3 and its result output at 0.5.
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

    int wire(int fromId, int fromPort, int toId, int toPort)
    {
        core::Connection connection;
        connection.fromNodeId = fromId;
        connection.fromPortIndex = fromPort;
        connection.toNodeId = toId;
        connection.toPortIndex = toPort;
        const int id = layer.graph().addConnection(connection);
        return id;
    }

    // A world point on the wire between the two nodes' ports.
    QPointF wireMidpoint(int connectionId)
    {
        const core::Connection *c = layer.graph().connectionById(connectionId);
        const core::Node *from = layer.graph().nodeById(c->fromNodeId);
        const core::Node *to = layer.graph().nodeById(c->toNodeId);
        const QVector<QPointF> samples = core::sampleConnector(
            from->portWorldPosition(c->fromPortIndex),
            to->portWorldPosition(c->toPortIndex));
        return samples[samples.size() / 2];
    }
};

} // namespace

class CanvasToolsTest : public QObject {
    Q_OBJECT

private slots:
    void cutToolSlicesConnectorUndoably()
    {
        Board board;
        const int aId = board.addDefaultNode(QPointF(300, 300));
        const int bId = board.addDefaultNode(QPointF(900, 320));
        const int wireId = board.wire(aId, 3, bId, 0);
        QCOMPARE(board.layer.graph().connections().size(), 1);

        board.layer.setTool(render::NodeLayerItem::Tool::Cut);
        const QPointF slicePoint = board.wireMidpoint(wireId);
        press(board.layer, slicePoint);
        release(board.layer, slicePoint);
        QVERIFY(board.layer.graph().connections().isEmpty());
        // The nodes themselves are untouched, and the slice is one undo away.
        QCOMPARE(board.layer.graph().nodes().size(), 2);
        board.layer.undo();
        QCOMPARE(board.layer.graph().connections().size(), 1);
    }

    void cutStrokeSlicesEveryConnectorItCrosses()
    {
        Board board;
        const int aId = board.addDefaultNode(QPointF(300, 250));
        const int bId = board.addDefaultNode(QPointF(900, 250));
        const int cId = board.addDefaultNode(QPointF(300, 700));
        const int dId = board.addDefaultNode(QPointF(900, 700));
        const int topWire = board.wire(aId, 3, bId, 0);
        const int bottomWire = board.wire(cId, 3, dId, 0);

        board.layer.setTool(render::NodeLayerItem::Tool::Cut);
        press(board.layer, QPointF(600, 100));
        move(board.layer, board.wireMidpoint(topWire));
        QCOMPARE(board.layer.graph().connections().size(), 1);
        move(board.layer, board.wireMidpoint(bottomWire));
        release(board.layer, QPointF(600, 900));
        QVERIFY(board.layer.graph().connections().isEmpty());
    }

    void placeToolDropsPrototypeAndFallsBackToCursor()
    {
        Board board;
        QSignalSpy toolSpy(&board.layer, &render::NodeLayerItem::toolChanged);
        board.layer.armPlacement(core::catalogPrototype(QStringLiteral("Prompt")));
        QCOMPARE(board.layer.tool(), render::NodeLayerItem::Tool::Place);
        QCOMPARE(toolSpy.count(), 1);

        const QPointF drop(640, 420);
        press(board.layer, drop);
        release(board.layer, drop);

        QCOMPARE(board.layer.graph().nodes().size(), 1);
        const core::Node &placed = board.layer.graph().nodes().first();
        QCOMPARE(placed.kind, core::NodeKind::Prompt);
        QCOMPARE(placed.worldRect().center(), drop);
        QVERIFY(placed.selected);
        QCOMPARE(board.layer.tool(), render::NodeLayerItem::Tool::Cursor);
        QCOMPARE(toolSpy.count(), 2);

        board.layer.undo();
        QVERIFY(board.layer.graph().nodes().isEmpty());
    }

    void connectToolWiresNodeBodies()
    {
        Board board;
        const int aId = board.addDefaultNode(QPointF(300, 300));
        const int bId = board.addDefaultNode(QPointF(1000, 600));

        board.layer.setTool(render::NodeLayerItem::Tool::Connect);
        drag(board.layer, QPointF(300, 300), QPointF(1000, 600));

        QCOMPARE(board.layer.graph().connections().size(), 1);
        const core::Connection &wire = board.layer.graph().connections().first();
        QCOMPARE(wire.fromNodeId, aId);
        QCOMPARE(wire.fromPortIndex, 3); // the node's result output
        QCOMPARE(wire.toNodeId, bId);
        QCOMPARE(wire.toPortIndex, 0); // best direct match: the image input
    }

    void frameCarriesRestingNodesWithoutSelectingThem()
    {
        Board board;
        board.layer.placePrototypeAt(core::catalogPrototype(QStringLiteral("Frame")),
                                     QPointF(500, 400));
        const int frameId = board.layer.graph().nodes().last().id;
        const int insideId = board.addDefaultNode(QPointF(500, 400));
        const int outsideId = board.addDefaultNode(QPointF(1300, 800));

        const QPointF insideBefore =
            board.layer.graph().nodeById(insideId)->worldPos;
        const QPointF outsideBefore =
            board.layer.graph().nodeById(outsideId)->worldPos;

        // Grab the frame on its own surface, clear of the resting node.
        drag(board.layer, QPointF(250, 250), QPointF(350, 370));

        const QPointF delta(100, 120);
        QCOMPARE(board.layer.graph().nodeById(insideId)->worldPos,
                 insideBefore + delta);
        QCOMPARE(board.layer.graph().nodeById(outsideId)->worldPos, outsideBefore);
        QVERIFY(board.layer.graph().nodeById(frameId)->selected);
        QVERIFY(!board.layer.graph().nodeById(insideId)->selected);

        // The whole carry is one undo step.
        board.layer.undo();
        QCOMPARE(board.layer.graph().nodeById(insideId)->worldPos, insideBefore);
    }

    void nodesPickOverFramesWhereverTheyOverlap()
    {
        Board board;
        board.layer.placePrototypeAt(core::catalogPrototype(QStringLiteral("Frame")),
                                     QPointF(500, 400));
        const int nodeId = board.addDefaultNode(QPointF(500, 400));

        press(board.layer, QPointF(500, 400));
        release(board.layer, QPointF(500, 400));
        QVERIFY(board.layer.graph().nodeById(nodeId)->selected);
    }

    void paletteOpensFromTabAndEmptyDoubleClick()
    {
        Board board;
        QSignalSpy spy(&board.layer, &render::NodeLayerItem::paletteInvoked);

        doubleClick(board.layer, QPointF(420, 260));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().first().toPointF(), QPointF(420, 260));

        key(board.layer, Qt::Key_Tab, Qt::NoModifier);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.last().first().toPointF(), QPointF(800, 500));

        // A frame backdrop counts as empty canvas: double-clicking its
        // surface summons the palette rather than doing nothing.
        board.layer.placePrototypeAt(core::catalogPrototype(QStringLiteral("Frame")),
                                     QPointF(500, 400));
        doubleClick(board.layer, QPointF(300, 300));
        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.last().first().toPointF(), QPointF(300, 300));
    }

    void generateNodeDoubleClickOpensPromptEditor()
    {
        Board board;
        const int nodeId = board.addDefaultNode(QPointF(500, 400));
        QSignalSpy spy(&board.layer, &render::NodeLayerItem::promptEditRequested);
        doubleClick(board.layer, QPointF(500, 400));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toInt(), nodeId);
    }

    void escapeReturnsTheToolToCursor()
    {
        Board board;
        board.layer.setTool(render::NodeLayerItem::Tool::Cut);
        key(board.layer, Qt::Key_Escape, Qt::NoModifier);
        QCOMPARE(board.layer.tool(), render::NodeLayerItem::Tool::Cursor);
    }

    void subgraphPlacementLandsCenteredAsOneStep()
    {
        Board board;
        QVector<core::Node> protos = {
            core::catalogPrototype(QStringLiteral("Prompt")),
            core::catalogPrototype(QStringLiteral("Generate Image")),
        };
        protos[1].worldPos = QPointF(400, 40);
        core::Connection wire;
        wire.fromNodeId = 0;
        wire.fromPortIndex = 0;
        wire.toNodeId = 1;
        wire.toPortIndex = 1;

        const QVector<int> ids = board.layer.placeSubgraphAt(
            protos, { wire }, QPointF(800, 500));
        QCOMPARE(ids.size(), 2);
        QCOMPARE(board.layer.graph().connections().size(), 1);
        QRectF bounds;
        for (const core::Node &node : board.layer.graph().nodes())
            bounds = bounds.isNull() ? node.worldRect()
                                     : bounds.united(node.worldRect());
        QCOMPARE(bounds.center(), QPointF(800, 500));

        board.layer.undo();
        QVERIFY(board.layer.graph().nodes().isEmpty());
        QVERIFY(board.layer.graph().connections().isEmpty());
    }

    void undoRedoSeamsMirrorTheKeyboard()
    {
        Board board;
        QVERIFY(!board.layer.canUndo());
        board.addDefaultNode(QPointF(400, 300));
        QVERIFY(board.layer.canUndo());
        QVERIFY(!board.layer.canRedo());

        QSignalSpy boardSpy(&board.layer, &render::NodeLayerItem::boardChanged);
        board.layer.undo();
        QVERIFY(board.layer.graph().nodes().isEmpty());
        QVERIFY(board.layer.canRedo());
        board.layer.redo();
        QCOMPARE(board.layer.graph().nodes().size(), 1);
        QVERIFY(boardSpy.count() >= 2);
    }

    void minimapMirrorsBoardCameraAndNavigates()
    {
        Board board;
        DrivableMinimap minimap;
        minimap.setSize(QSizeF(220, 150));
        minimap.setLayer(&board.layer);
        minimap.setController(&board.controller);

        const int leftId = board.addDefaultNode(QPointF(200, 300));
        const int rightId = board.addDefaultNode(QPointF(2600, 900));

        // Blocks keep the world's relative arrangement.
        const QRectF leftBlock = minimap.blockRectFor(leftId);
        const QRectF rightBlock = minimap.blockRectFor(rightId);
        QVERIFY(leftBlock.center().x() < rightBlock.center().x());
        QVERIFY(leftBlock.center().y() < rightBlock.center().y());

        // The block is exactly the projected node rect.
        const core::Node *leftNode = board.layer.graph().nodeById(leftId);
        QCOMPARE(leftBlock,
                 minimap.projection().miniFromWorld(leftNode->worldRect()));

        // Moving a node on the canvas moves its block and announces the change.
        QSignalSpy boardSpy(&board.layer, &render::NodeLayerItem::boardChanged);
        drag(board.layer, QPointF(200, 300), QPointF(200, 700));
        QVERIFY(boardSpy.count() >= 1);
        QVERIFY(minimap.blockRectFor(leftId).center().y()
                > leftBlock.center().y());

        // Panning the camera moves the viewport rectangle.
        const QRectF viewBefore = minimap.viewportMiniRect();
        board.controller.panByPixels(QPointF(-400, 0));
        const QRectF viewAfter = minimap.viewportMiniRect();
        QVERIFY(viewAfter.center().x() > viewBefore.center().x());

        // A click inside the minimap recenters the camera on that world point.
        QMouseEvent click(QEvent::MouseButtonPress,
                          minimap.blockRectFor(rightId).center(),
                          minimap.blockRectFor(rightId).center(), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        minimap.mousePressEvent(&click);
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease,
                                 minimap.blockRectFor(rightId).center(),
                                 minimap.blockRectFor(rightId).center(),
                                 Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        minimap.mouseReleaseEvent(&releaseEvent);

        const core::Node *rightNode = board.layer.graph().nodeById(rightId);
        const QPointF centred = board.controller.screenFromWorld(
            rightNode->worldRect().center(), 1.0);
        QVERIFY(qAbs(centred.x() - 800.0) < 12.0);
        QVERIFY(qAbs(centred.y() - 500.0) < 12.0);
    }

    void minimapProjectionSurvivesEmptyBoard()
    {
        Board board;
        DrivableMinimap minimap;
        minimap.setSize(QSizeF(220, 150));
        minimap.setLayer(&board.layer);
        minimap.setController(&board.controller);

        // No nodes: the projection frames the camera's viewport and stays
        // invertible.
        const QPointF world = minimap.worldAtItemPos(QPointF(110, 75));
        const QPointF back = minimap.projection().miniFromWorld(world);
        QVERIFY(qAbs(back.x() - 110.0) < 1e-6);
        QVERIFY(qAbs(back.y() - 75.0) < 1e-6);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    CanvasToolsTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_canvastools.moc"
