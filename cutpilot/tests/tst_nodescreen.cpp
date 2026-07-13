#include <QtTest/QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include "CanvasCluster.h"
#include "CommandPalette.h"
#include "RailPanels.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/MinimapItem.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

using namespace cutpilot;
using cutpilot::app::CanvasCluster;
using cutpilot::app::CommandPalette;
using cutpilot::app::ContentPanel;
using cutpilot::app::PaletteModel;
using cutpilot::app::SearchPanel;

// The Node screen as one working whole, over the live stack: the command
// palette, the canvas layer and camera, the generation coordinator against a
// real sidecar, the minimap mapping, the history cluster, and the rail's
// Content and Search panels — each element exercised through the others, not
// in isolation.
class NodeScreenTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void paletteAddGeneratesMapsUndoesAndReachesContent();
    void searchSpansLiveNodesAndTheModelRegistry();

private:
    struct Rig {
        theme::ThemeTable table{ theme::Theme::Dark };
        render::CanvasController controller;
        render::NodeLayerItem layer;
        render::MinimapItem minimap;
        ipc::GenerationCoordinator coordinator;
        CommandPalette palette;
        CanvasCluster cluster;
        ContentPanel content;

        explicit Rig(ipc::GenerationClient *client)
            : coordinator(&layer.graph(), client)
            , palette(table, nullptr)
            , cluster(table, &layer, &controller, nullptr)
            , content(table, &layer, nullptr)
        {
            layer.setSize(QSizeF(1600, 1000));
            layer.setController(&controller);
            minimap.setSize(QSizeF(220, 150));
            minimap.setLayer(&layer);
            minimap.setController(&controller);

            // The window's wiring, reproduced: a palette pick lands on the
            // canvas as an undoable placement, decoded results become card
            // media, and the registry feeds the palette rows.
            QObject::connect(&palette, &CommandPalette::prototypeChosen,
                             &layer, [this](const core::Node &prototype) {
                                 layer.placePrototypeAt(prototype,
                                                        QPointF(600.0, 400.0));
                             });
            QObject::connect(&coordinator,
                             &ipc::GenerationCoordinator::nodeMediaReady,
                             &layer,
                             [this](int nodeId, const QImage &image) {
                                 layer.setNodeMedia(nodeId, image);
                             });
            QObject::connect(&coordinator,
                             &ipc::GenerationCoordinator::modelsReady,
                             &palette, [this] {
                                 QVector<PaletteModel::ModelEntry> entries;
                                 for (const auto &model : coordinator.models())
                                     entries.push_back({ model.id, model.label,
                                                         model.provider,
                                                         model.hasKey });
                                 palette.model().setModels(entries);
                             });
        }
    };

    bool waitForModels(Rig &rig)
    {
        QSignalSpy modelsSpy(&rig.coordinator,
                             &ipc::GenerationCoordinator::modelsReady);
        rig.coordinator.serviceBecameReady();
        return QTest::qWaitFor([&] { return modelsSpy.count() >= 1; }, 10000);
    }

    QTemporaryDir m_genDir;
    ipc::SidecarHost m_host;
    ipc::GenerationClient m_client;
};

void NodeScreenTest::initTestCase()
{
    qputenv("CUTPILOT_DISABLE_KEYCHAIN", "1");
    qunsetenv("OPENAI_API_KEY");
    QVERIFY(m_genDir.isValid());
    qputenv("CUTPILOT_GEN_DIR", m_genDir.path().toUtf8());

    QSignalSpy readySpy(&m_host, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&m_host, &ipc::SidecarHost::failed);
    m_host.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1,
                             15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    m_client.setEndpoint(m_host.port(), m_host.token());
}

void NodeScreenTest::cleanupTestCase()
{
    m_host.stop();
}

void NodeScreenTest::paletteAddGeneratesMapsUndoesAndReachesContent()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    // A node enters through the palette's real keyboard flow.
    rig.palette.open();
    auto *search = rig.palette.findChild<QLineEdit *>();
    QVERIFY(search);
    QTest::keyClicks(search, QStringLiteral("generate image"));
    QTest::keyClick(search, Qt::Key_Return);
    QCOMPARE(rig.layer.graph().nodes().size(), 1);
    const core::Node &placed = rig.layer.graph().nodes().first();
    const int nodeId = placed.id;
    const QString uid = placed.uid;
    QCOMPARE(placed.kind, core::NodeKind::Generate);
    QVERIFY(!uid.isEmpty());

    // Fresh from the palette the node carries no model yet: a run refuses
    // with guidance instead of submitting blind.
    rig.layer.setNodePrompt(nodeId, QStringLiteral("a slate-grey harbor"));
    rig.coordinator.runNode(nodeId);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->runState,
             core::RunState::Error);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->statusMessage,
             QStringLiteral("Pick a model"));

    // Picking through the undoable picker seam and running generates through
    // the shared engine on the offline model — the same lifecycle a keyed
    // vendor run streams.
    rig.layer.setNodeModel(nodeId, QStringLiteral("local/procedural-v1"),
                           QStringLiteral("Procedural (local)"));
    rig.coordinator.runNode(nodeId);
    QTRY_COMPARE_WITH_TIMEOUT(rig.layer.graph().nodeById(nodeId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.coordinator.runActive(), 10000);
    const core::Node *done = rig.layer.graph().nodeById(nodeId);
    const QString resultPath = done->resultPath;
    const QPointF worldCentre = done->worldRect().center();
    QVERIFY(QFile::exists(resultPath));
    QVERIFY(done->costUsd > 0.0);
    QVERIFY(done->resultWidth > 0 && done->resultHeight > 0);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.layer.nodeMediaImage(nodeId).isNull(),
                             10000);

    // The minimap frames it, tinted by its media, and its block maps back
    // to the node — the mapping a minimap click pans the camera through.
    const QRectF block = rig.minimap.blockRectFor(nodeId);
    QVERIFY(!block.isEmpty());
    QVERIFY(QRectF(0, 0, 220, 150).contains(block));
    QVERIFY(rig.layer.nodeAverageColor(nodeId).isValid());
    const QPointF mappedWorld = rig.minimap.worldAtItemPos(block.center());
    QVERIFY((mappedWorld - worldCentre).manhattanLength() < 40.0);
    rig.controller.centerOnWorld(mappedWorld, QSizeF(1600, 1000), 1.0);
    const QPointF onScreen =
        rig.controller.screenFromWorld(mappedWorld, 1.0);
    QVERIFY((onScreen - QPointF(800.0, 500.0)).manhattanLength() < 1.0);

    // The cluster's history controls drive the same stack the palette
    // placement landed on: undo removes the node everywhere, redo restores
    // it under the same identity with its result still attached.
    QToolButton *undo = nullptr;
    QToolButton *redo = nullptr;
    for (QToolButton *button : rig.cluster.findChildren<QToolButton *>()) {
        if (button->toolTip().startsWith(QStringLiteral("Undo")))
            undo = button;
        if (button->toolTip().startsWith(QStringLiteral("Redo")))
            redo = button;
    }
    QVERIFY(undo && redo);
    QVERIFY(undo->isEnabled());
    undo->click(); // the model pick
    undo->click(); // the prompt edit
    undo->click(); // the placement
    QVERIFY(!rig.layer.graph().nodeById(nodeId));
    QVERIFY(rig.minimap.blockRectFor(nodeId).isEmpty());
    rig.content.refresh();
    QVERIFY(rig.content.findChild<QTreeWidget *>()->topLevelItemCount() == 0);

    QVERIFY(redo->isEnabled());
    redo->click();
    redo->click();
    redo->click();
    const core::Node *restored = rig.layer.graph().nodeById(nodeId);
    QVERIFY(restored);
    QCOMPARE(restored->uid, uid);
    QCOMPARE(restored->resultPath, resultPath);
    QVERIFY(!rig.layer.nodeMediaImage(nodeId).isNull());

    // Content lists the restored node and clicking its row announces the
    // jump the camera serves.
    rig.content.show();
    rig.content.refresh();
    auto *tree = rig.content.findChild<QTreeWidget *>();
    QVERIFY(tree);
    QTreeWidgetItem *row = nullptr;
    for (int g = 0; g < tree->topLevelItemCount() && !row; ++g) {
        QTreeWidgetItem *group = tree->topLevelItem(g);
        for (int i = 0; i < group->childCount() && !row; ++i) {
            if (group->child(i)->text(0).startsWith(
                    QStringLiteral("Generate Image")))
                row = group->child(i);
        }
    }
    QVERIFY(row);
    QSignalSpy activated(&rig.content, &ContentPanel::nodeActivated);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::KeyboardModifiers(),
                      tree->visualItemRect(row).center());
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.first().first().toInt(), nodeId);
}

void NodeScreenTest::searchSpansLiveNodesAndTheModelRegistry()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    core::Node prompt = core::Node();
    prompt.kind = core::NodeKind::Prompt;
    prompt.title = QStringLiteral("Opening beat");
    prompt.worldSize = QSizeF(260, 170);
    const int promptId = rig.layer.placePrototypeAt(prompt, QPointF(0, 0));

    SearchPanel panel(
        rig.table, &rig.layer,
        [&rig] {
            QVector<PaletteModel::ModelEntry> entries;
            for (const auto &model : rig.coordinator.models())
                entries.push_back({ model.id, model.label, model.provider,
                                    model.hasKey });
            return entries;
        },
        nullptr);
    panel.show();

    auto *field = panel.findChild<QLineEdit *>();
    auto *results = panel.findChild<QListWidget *>();
    QVERIFY(field && results);

    // A live node is found by its title and activates the camera jump. The
    // first row is the section header; the hit sits under it.
    field->setText(QStringLiteral("opening"));
    QTRY_VERIFY_WITH_TIMEOUT(results->count() >= 2, 2000);
    QCOMPARE(results->item(1)->text(), QStringLiteral("Opening beat"));
    QSignalSpy activated(&panel, &SearchPanel::nodeActivated);
    QTest::mouseClick(results->viewport(), Qt::LeftButton,
                      Qt::KeyboardModifiers(),
                      results->visualItemRect(results->item(1)).center());
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.first().first().toInt(), promptId);

    // A registry model is found by name and offers a ready-to-place node.
    field->setText(QStringLiteral("procedural upscale"));
    QTRY_VERIFY_WITH_TIMEOUT(results->count() >= 2, 2000);
    QSignalSpy chosen(&panel, &SearchPanel::prototypeChosen);
    QTest::mouseClick(results->viewport(), Qt::LeftButton,
                      Qt::KeyboardModifiers(),
                      results->visualItemRect(results->item(1)).center());
    QCOMPARE(chosen.count(), 1);
    const auto prototype = chosen.first().first().value<core::Node>();
    QCOMPARE(prototype.kind, core::NodeKind::Generate);
    QCOMPARE(prototype.modelId, QStringLiteral("local/procedural-upscale-v1"));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    NodeScreenTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_nodescreen.moc"
