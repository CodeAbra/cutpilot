#include <QtTest/QtTest>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/core/command/EditPromptCommand.h"
#include "cutpilot/core/command/SetModelCommand.h"

#include <memory>

using namespace cutpilot::core;

class GenerationCommandsTest : public QObject {
    Q_OBJECT

private slots:
    void editPromptAppliesAndUndoes();
    void setModelAppliesAndUndoes();
    void redoReplaysTheEdit();
    void contentRevisionAdvancesOnEveryDirection();
    void missingNodeIsANoOp();
};

namespace {

int addPromptNode(NodeGraph &graph)
{
    Node node;
    node.kind = NodeKind::Prompt;
    node.title = QStringLiteral("Prompt");
    node.promptText = QStringLiteral("first draft");
    node.worldSize = QSizeF(260, 170);
    return graph.addNode(node);
}

int addGenerateNode(NodeGraph &graph)
{
    Node node;
    node.kind = NodeKind::Generate;
    node.title = QStringLiteral("Generate Image");
    node.worldSize = QSizeF(280, 200);
    return graph.addNode(node);
}

} // namespace

void GenerationCommandsTest::editPromptAppliesAndUndoes()
{
    NodeGraph graph;
    CommandStack stack;
    const int id = addPromptNode(graph);

    stack.push(std::make_unique<EditPromptCommand>(id, QStringLiteral("night harbor")),
               graph);
    QCOMPARE(graph.nodeById(id)->promptText, QStringLiteral("night harbor"));

    stack.undo(graph);
    QCOMPARE(graph.nodeById(id)->promptText, QStringLiteral("first draft"));
}

void GenerationCommandsTest::setModelAppliesAndUndoes()
{
    NodeGraph graph;
    CommandStack stack;
    const int id = addGenerateNode(graph);

    stack.push(std::make_unique<SetModelCommand>(id, QStringLiteral("local/procedural-v1"),
                                                 QStringLiteral("Procedural (local)")),
               graph);
    QCOMPARE(graph.nodeById(id)->modelId, QStringLiteral("local/procedural-v1"));
    QCOMPARE(graph.nodeById(id)->modelLabel, QStringLiteral("Procedural (local)"));

    stack.undo(graph);
    QVERIFY(graph.nodeById(id)->modelId.isEmpty());
    QVERIFY(graph.nodeById(id)->modelLabel.isEmpty());
}

void GenerationCommandsTest::redoReplaysTheEdit()
{
    NodeGraph graph;
    CommandStack stack;
    const int id = addPromptNode(graph);

    stack.push(std::make_unique<EditPromptCommand>(id, QStringLiteral("v2")), graph);
    stack.push(std::make_unique<EditPromptCommand>(id, QStringLiteral("v3")), graph);
    stack.undo(graph);
    stack.undo(graph);
    QCOMPARE(graph.nodeById(id)->promptText, QStringLiteral("first draft"));

    stack.redo(graph);
    QCOMPARE(graph.nodeById(id)->promptText, QStringLiteral("v2"));
    stack.redo(graph);
    QCOMPARE(graph.nodeById(id)->promptText, QStringLiteral("v3"));
}

void GenerationCommandsTest::contentRevisionAdvancesOnEveryDirection()
{
    NodeGraph graph;
    CommandStack stack;
    const int id = addPromptNode(graph);

    int lastRevision = graph.nodeById(id)->contentRevision;
    stack.push(std::make_unique<EditPromptCommand>(id, QStringLiteral("edited")), graph);
    QVERIFY(graph.nodeById(id)->contentRevision > lastRevision);

    lastRevision = graph.nodeById(id)->contentRevision;
    stack.undo(graph);
    QVERIFY(graph.nodeById(id)->contentRevision > lastRevision);

    lastRevision = graph.nodeById(id)->contentRevision;
    stack.redo(graph);
    QVERIFY(graph.nodeById(id)->contentRevision > lastRevision);
}

void GenerationCommandsTest::missingNodeIsANoOp()
{
    NodeGraph graph;
    CommandStack stack;

    stack.push(std::make_unique<EditPromptCommand>(99, QStringLiteral("orphan")), graph);
    stack.push(std::make_unique<SetModelCommand>(99, QStringLiteral("m"),
                                                 QStringLiteral("M")),
               graph);
    stack.undo(graph);
    stack.undo(graph);
    QVERIFY(graph.nodes().isEmpty());
}

QTEST_GUILESS_MAIN(GenerationCommandsTest)
#include "tst_generationcommands.moc"
