#include <QtTest/QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "cutpilot/core/ComfyImport.h"

using namespace cutpilot::core;

namespace {

// The shape the convert service returns for a three-node workflow: a prompt
// feeding a sampler, plus one unknown node preserved whole.
QJsonObject importResult()
{
    return QJsonDocument::fromJson(R"({
        "nodes": [
            {"comfy_id": 1, "comfy_type": "CLIPTextEncode", "tier": "exact",
             "kind": "prompt", "title": "", "pos": [80, 40],
             "prompt": "a lighthouse at dusk", "media": "", "opaque": null},
            {"comfy_id": 2, "comfy_type": "KSampler", "tier": "substituted",
             "kind": "generate", "title": "", "pos": [400, 40],
             "prompt": "", "media": "", "opaque": null},
            {"comfy_id": 3, "comfy_type": "GlitterStorm",
             "tier": "passthrough", "kind": "blank", "title": "", "pos": [700, 40],
             "prompt": "", "media": "",
             "opaque": {"id": 3, "type": "GlitterStorm",
                        "properties": {"sparkle": true}}}
        ],
        "connections": [
            {"from": 1, "to": 2},
            {"from": 42, "to": 2}
        ],
        "report": [
            {"id": 1, "type": "CLIPTextEncode", "tier": "exact", "mapped": "prompt"},
            {"id": 2, "type": "KSampler", "tier": "substituted", "mapped": "generate"},
            {"id": 3, "type": "GlitterStorm", "tier": "passthrough", "mapped": "blank"}
        ]
    })")
        .object();
}

} // namespace

class ComfyImportTest : public QObject {
    Q_OBJECT

private slots:
    void importLandsMappedNodesAndWires();
    void unknownNodesArePreservedWhole();
    void importIsOneUndoStepAndRedoRestoresIt();
    void emptyResultRefusesWithoutTouchingTheGraph();
};

void ComfyImportTest::importLandsMappedNodesAndWires()
{
    NodeGraph graph;
    CommandStack commands;
    const ComfyImportOutcome outcome =
        applyComfyImport(graph, commands, importResult(), QPointF(100, 200));
    QVERIFY2(outcome.ok, qPrintable(outcome.error));

    QCOMPARE(outcome.nodeIds.size(), 3);
    QCOMPARE(graph.nodes().size(), 3);

    const Node *prompt = graph.nodeById(outcome.nodeIds[0]);
    QVERIFY(prompt);
    QCOMPARE(prompt->kind, NodeKind::Prompt);
    QCOMPARE(prompt->promptText, QStringLiteral("a lighthouse at dusk"));
    QCOMPARE(prompt->worldPos, QPointF(180, 240));

    const Node *generate = graph.nodeById(outcome.nodeIds[1]);
    QVERIFY(generate);
    QCOMPARE(generate->kind, NodeKind::Generate);

    // The surviving link wired text output to the prompt input; the link
    // from the missing node was dropped and counted.
    QCOMPARE(outcome.connectionCount, 1);
    QCOMPARE(outcome.droppedEdges, 1);
    QCOMPARE(graph.connections().size(), 1);
    const Connection &wire = graph.connections().first();
    QCOMPARE(wire.fromNodeId, prompt->id);
    QCOMPARE(wire.toNodeId, generate->id);
    QCOMPARE(generate->ports.at(wire.toPortIndex).type, PortType::Text);

    // The report rode through untouched.
    QCOMPARE(outcome.report.size(), 3);
    QCOMPARE(outcome.report[0].tier, QStringLiteral("exact"));
    QCOMPARE(outcome.report[1].tier, QStringLiteral("substituted"));
    QCOMPARE(outcome.report[2].tier, QStringLiteral("passthrough"));
}

void ComfyImportTest::unknownNodesArePreservedWhole()
{
    NodeGraph graph;
    CommandStack commands;
    const ComfyImportOutcome outcome =
        applyComfyImport(graph, commands, importResult());
    QVERIFY(outcome.ok);

    const Node *foreign = graph.nodeById(outcome.nodeIds[2]);
    QVERIFY(foreign);
    QCOMPARE(foreign->kind, NodeKind::Blank);
    QCOMPARE(foreign->externalType, QStringLiteral("GlitterStorm"));
    QCOMPARE(foreign->title, QStringLiteral("GlitterStorm"));

    const QJsonObject preserved =
        QJsonDocument::fromJson(foreign->externalData.toUtf8()).object();
    QCOMPARE(preserved.value(QStringLiteral("type")).toString(),
             QStringLiteral("GlitterStorm"));
    QCOMPARE(preserved.value(QStringLiteral("properties"))
                 .toObject()
                 .value(QStringLiteral("sparkle"))
                 .toBool(),
             true);
}

void ComfyImportTest::importIsOneUndoStepAndRedoRestoresIt()
{
    NodeGraph graph;
    CommandStack commands;
    const ComfyImportOutcome outcome =
        applyComfyImport(graph, commands, importResult());
    QVERIFY(outcome.ok);

    commands.undo(graph);
    QVERIFY(graph.nodes().isEmpty());
    QVERIFY(graph.connections().isEmpty());

    commands.redo(graph);
    QCOMPARE(graph.nodes().size(), 3);
    QCOMPARE(graph.connections().size(), 1);
    for (int id : outcome.nodeIds)
        QVERIFY(graph.nodeById(id));
    const Connection &wire = graph.connections().first();
    QCOMPARE(wire.fromNodeId, outcome.nodeIds[0]);
    QCOMPARE(wire.toNodeId, outcome.nodeIds[1]);
}

void ComfyImportTest::emptyResultRefusesWithoutTouchingTheGraph()
{
    NodeGraph graph;
    CommandStack commands;
    const ComfyImportOutcome outcome =
        applyComfyImport(graph, commands, QJsonObject());
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.error.isEmpty());
    QVERIFY(graph.nodes().isEmpty());
    QCOMPARE(commands.depth(), 0);
}

QTEST_GUILESS_MAIN(ComfyImportTest)
#include "tst_comfyimport.moc"
