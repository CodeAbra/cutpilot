#include <QtTest/QtTest>

#include <QSettings>
#include <QSignalSpy>

#include "ThemeController.h"
#include "WorkflowTemplates.h"
#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"

using namespace cutpilot;
using cutpilot::app::ThemeController;
using cutpilot::app::WorkflowTemplate;

namespace {

// A board where a finished run and a media source are both selected — the
// everyday moment someone saves a reusable block.
core::NodeGraph boardWithResults()
{
    core::NodeGraph graph;

    core::Node generate = core::catalogPrototype(QStringLiteral("Generate Image"));
    generate.worldPos = QPointF(400.0, 100.0);
    generate.runState = core::RunState::Done;
    generate.runProgress = 1.0;
    generate.statusMessage = QStringLiteral("done");
    generate.resultPath = QStringLiteral("/home/someone/results/render.png");
    generate.resultDigest = QStringLiteral("deadbeef");
    generate.costUsd = 0.42;
    generate.estimatedCostUsd = 0.4;
    generate.resultWidth = 768;
    generate.resultHeight = 512;
    const int generateId = graph.addNode(generate);

    core::Node still = core::compositeNodePrototype(core::NodeKind::Still);
    still.worldPos = QPointF(0.0, 100.0);
    still.mediaPath = QStringLiteral("/assets/plate.png");
    const int stillId = graph.addNode(still);

    core::Connection wire;
    wire.fromNodeId = stillId;
    wire.fromPortIndex = 0;
    wire.toNodeId = generateId;
    wire.toPortIndex = 0;
    graph.addConnection(wire);

    graph.setSelected(generateId, true);
    graph.setSelected(stillId, true);
    return graph;
}

} // namespace

class WorkflowTemplatesTest : public QObject {
    Q_OBJECT

private slots:
    void captureStripsRunArtifactsButKeepsSources()
    {
        const core::NodeGraph graph = boardWithResults();
        const WorkflowTemplate content = app::captureSelectionTemplate(graph);

        QCOMPARE(content.prototypes.size(), 2);
        QCOMPARE(content.indexWires.size(), 1);

        for (const core::Node &prototype : content.prototypes) {
            // A template is pristine: no ids, selection, run state, results,
            // costs, or a past run's local file paths.
            QCOMPARE(prototype.id, 0);
            QVERIFY(!prototype.selected);
            QCOMPARE(prototype.runState, core::RunState::Idle);
            QCOMPARE(prototype.runProgress, 0.0);
            QVERIFY(prototype.statusMessage.isEmpty());
            QVERIFY(prototype.resultPath.isEmpty());
            QVERIFY(prototype.resultDigest.isEmpty());
            QCOMPARE(prototype.costUsd, -1.0);
            QCOMPARE(prototype.estimatedCostUsd, -1.0);
            QCOMPARE(prototype.resultWidth, 0);
            QCOMPARE(prototype.resultHeight, 0);
        }

        // The media source keeps its path — it is the node's whole point.
        QCOMPARE(content.prototypes[1].kind, core::NodeKind::Still);
        QCOMPARE(content.prototypes[1].mediaPath,
                 QStringLiteral("/assets/plate.png"));

        // Positions are normalized to the joint origin; wires re-express by
        // prototype index.
        QCOMPARE(content.prototypes[1].worldPos, QPointF(0.0, 0.0));
        QCOMPARE(content.indexWires.first().fromNodeId, 1);
        QCOMPARE(content.indexWires.first().toNodeId, 0);
    }

    void captureIgnoresUnselectedNodesAndOutboundWires()
    {
        core::NodeGraph graph = boardWithResults();
        core::Node outsider = core::catalogPrototype(QStringLiteral("Prompt"));
        graph.addNode(outsider); // not selected

        const WorkflowTemplate content = app::captureSelectionTemplate(graph);
        QCOMPARE(content.prototypes.size(), 2);
        for (const core::Node &prototype : content.prototypes)
            QVERIFY(prototype.kind != core::NodeKind::Prompt);
    }

    void templateJsonRoundTrips()
    {
        const WorkflowTemplate saved =
            app::captureSelectionTemplate(boardWithResults());
        const QJsonObject json =
            app::templateToJson(saved, QStringLiteral("Plate chain"));

        WorkflowTemplate restored;
        QString name;
        QVERIFY(app::templateFromJson(json, restored, &name));
        QCOMPARE(name, QStringLiteral("Plate chain"));
        QCOMPARE(restored.prototypes.size(), saved.prototypes.size());
        QCOMPARE(restored.indexWires.size(), saved.indexWires.size());
        QCOMPARE(restored.prototypes[1].mediaPath,
                 QStringLiteral("/assets/plate.png"));
        QVERIFY(restored.prototypes[0].resultPath.isEmpty());
        QCOMPARE(restored.indexWires.first().fromNodeId, 1);
        QCOMPARE(restored.indexWires.first().toPortIndex, 0);
    }

    void themeControllerCyclesAndPersists()
    {
        QSettings().remove(QStringLiteral("appearance/theme"));

        ThemeController controller;
        QCOMPARE(controller.theme(), theme::Theme::Dark);

        QSignalSpy changes(&controller, &ThemeController::themeChanged);
        controller.cycle();
        QCOMPARE(controller.theme(), theme::Theme::Light);
        controller.cycle();
        QCOMPARE(controller.theme(), theme::Theme::DarkDim);
        controller.cycle();
        QCOMPARE(controller.theme(), theme::Theme::Dark);
        QCOMPARE(changes.count(), 3);

        // The choice survives into a fresh controller.
        controller.setTheme(theme::Theme::Light);
        ThemeController fresh;
        QCOMPARE(fresh.theme(), theme::Theme::Light);
        QCOMPARE(fresh.themeName(), QStringLiteral("Light"));

        QSettings().remove(QStringLiteral("appearance/theme"));
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("CutPilotTests"));
    app.setApplicationName(QStringLiteral("workflow-templates"));
    WorkflowTemplatesTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_workflowtemplates.moc"
