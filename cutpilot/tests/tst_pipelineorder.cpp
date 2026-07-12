#include <QtTest/QtTest>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/PipelineOrder.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/core/command/SetGateLimitCommand.h"

#include <memory>

using namespace cutpilot::core;

namespace {

int addNode(NodeGraph &graph, NodeKind kind)
{
    Node node;
    node.kind = kind;
    node.worldSize = QSizeF(200, 150);
    node.ports = {
        { QStringLiteral("in"), PortType::Image, true, 0.4 },
        { QStringLiteral("in2"), PortType::Image, true, 0.7 },
        { QStringLiteral("out"), PortType::Image, false, 0.5 },
    };
    return graph.addNode(node);
}

// Wire from's output into the given input port of to.
void wire(NodeGraph &graph, int from, int to, int toPort = 0)
{
    Connection edge;
    edge.fromNodeId = from;
    edge.fromPortIndex = 2;
    edge.toNodeId = to;
    edge.toPortIndex = toPort;
    QVERIFY(graph.addConnection(edge) != -1);
}

QVector<int> evaluatableIds(const NodeGraph &graph)
{
    QVector<int> ids;
    for (const Node &node : graph.nodes()) {
        if (isEvaluatable(node))
            ids.push_back(node.id);
    }
    return ids;
}

int positionOf(const QVector<int> &order, int id)
{
    const int index = order.indexOf(id);
    return index;
}

} // namespace

class PipelineOrderTest : public QObject {
    Q_OBJECT

private slots:
    void chainOrdersDependencyFirst();
    void diamondRespectsBothArms();
    void passThroughNodesCarryDependencies();
    void promptInputsAreNotRunDependencies();
    void directCycleIsRefused();
    void cycleThroughAPassThroughIsRefused();
    void subsetTreatsOutsideDependenciesAsSatisfied();
    void closuresWalkTheWires();
    void closuresSurviveACycle();
    void gateLimitCommandAppliesUndoesAndRedoes();
};

void PipelineOrderTest::chainOrdersDependencyFirst()
{
    NodeGraph graph;
    const int a = addNode(graph, NodeKind::Generate);
    const int b = addNode(graph, NodeKind::Generate);
    const int c = addNode(graph, NodeKind::Generate);
    wire(graph, c, b); // declare out of order on purpose
    wire(graph, b, a);

    const EvaluationPlan plan = evaluationOrder(graph, evaluatableIds(graph));
    QVERIFY(!plan.hasCycle);
    QCOMPARE(plan.order.size(), 3);
    QVERIFY(positionOf(plan.order, c) < positionOf(plan.order, b));
    QVERIFY(positionOf(plan.order, b) < positionOf(plan.order, a));
}

void PipelineOrderTest::diamondRespectsBothArms()
{
    NodeGraph graph;
    const int top = addNode(graph, NodeKind::Generate);
    const int left = addNode(graph, NodeKind::Generate);
    const int right = addNode(graph, NodeKind::Generate);
    const int bottom = addNode(graph, NodeKind::Generate);
    wire(graph, top, left);
    wire(graph, top, right);
    wire(graph, left, bottom, 0);
    wire(graph, right, bottom, 1);

    const EvaluationPlan plan = evaluationOrder(graph, evaluatableIds(graph));
    QVERIFY(!plan.hasCycle);
    QVERIFY(positionOf(plan.order, top) < positionOf(plan.order, left));
    QVERIFY(positionOf(plan.order, top) < positionOf(plan.order, right));
    QVERIFY(positionOf(plan.order, left) < positionOf(plan.order, bottom));
    QVERIFY(positionOf(plan.order, right) < positionOf(plan.order, bottom));
}

void PipelineOrderTest::passThroughNodesCarryDependencies()
{
    NodeGraph graph;
    const int producer = addNode(graph, NodeKind::Generate);
    const int relay = addNode(graph, NodeKind::Blank);
    const int consumer = addNode(graph, NodeKind::Generate);
    wire(graph, producer, relay);
    wire(graph, relay, consumer);

    const QHash<int, QSet<int>> deps = generationDependencies(graph);
    QVERIFY(deps.value(consumer).contains(producer));
    QVERIFY(deps.value(producer).isEmpty());
}

void PipelineOrderTest::promptInputsAreNotRunDependencies()
{
    NodeGraph graph;
    const int prompt = addNode(graph, NodeKind::Prompt);
    const int generate = addNode(graph, NodeKind::Generate);
    wire(graph, prompt, generate);

    const QHash<int, QSet<int>> deps = generationDependencies(graph);
    QVERIFY(deps.value(generate).isEmpty());
}

void PipelineOrderTest::directCycleIsRefused()
{
    NodeGraph graph;
    const int a = addNode(graph, NodeKind::Generate);
    const int b = addNode(graph, NodeKind::Generate);
    wire(graph, a, b);
    wire(graph, b, a);

    const EvaluationPlan plan = evaluationOrder(graph, evaluatableIds(graph));
    QVERIFY(plan.hasCycle);
}

void PipelineOrderTest::cycleThroughAPassThroughIsRefused()
{
    NodeGraph graph;
    const int generate = addNode(graph, NodeKind::Generate);
    const int relay = addNode(graph, NodeKind::Blank);
    wire(graph, generate, relay);
    wire(graph, relay, generate);

    const EvaluationPlan plan = evaluationOrder(graph, evaluatableIds(graph));
    QVERIFY(plan.hasCycle);
}

void PipelineOrderTest::subsetTreatsOutsideDependenciesAsSatisfied()
{
    NodeGraph graph;
    const int upstream = addNode(graph, NodeKind::Generate);
    const int target = addNode(graph, NodeKind::Generate);
    wire(graph, upstream, target);

    const EvaluationPlan plan = evaluationOrder(graph, { target });
    QVERIFY(!plan.hasCycle);
    QCOMPARE(plan.order, QVector<int>{ target });
}

void PipelineOrderTest::closuresWalkTheWires()
{
    NodeGraph graph;
    const int prompt = addNode(graph, NodeKind::Prompt);
    const int generate = addNode(graph, NodeKind::Generate);
    const int upscale = addNode(graph, NodeKind::Generate);
    const int stranger = addNode(graph, NodeKind::Generate);
    wire(graph, prompt, generate);
    wire(graph, generate, upscale);

    const QSet<int> up = upstreamClosure(graph, upscale);
    QCOMPARE(up, QSet<int>({ prompt, generate, upscale }));

    const QSet<int> down = downstreamClosure(graph, prompt);
    QCOMPARE(down, QSet<int>({ prompt, generate, upscale }));

    QCOMPARE(upstreamClosure(graph, stranger), QSet<int>({ stranger }));
}

void PipelineOrderTest::closuresSurviveACycle()
{
    NodeGraph graph;
    const int a = addNode(graph, NodeKind::Generate);
    const int b = addNode(graph, NodeKind::Generate);
    wire(graph, a, b);
    wire(graph, b, a);

    QCOMPARE(upstreamClosure(graph, a), QSet<int>({ a, b }));
    QCOMPARE(downstreamClosure(graph, a), QSet<int>({ a, b }));
}

void PipelineOrderTest::gateLimitCommandAppliesUndoesAndRedoes()
{
    NodeGraph graph;
    CommandStack stack;
    const int gate = addNode(graph, NodeKind::CostGate);
    const double initial = graph.nodeById(gate)->gateLimitUsd;
    const int revision = graph.nodeById(gate)->contentRevision;

    stack.push(std::make_unique<SetGateLimitCommand>(gate, 0.25), graph);
    QCOMPARE(graph.nodeById(gate)->gateLimitUsd, 0.25);
    QVERIFY(graph.nodeById(gate)->contentRevision > revision);

    stack.undo(graph);
    QCOMPARE(graph.nodeById(gate)->gateLimitUsd, initial);

    stack.redo(graph);
    QCOMPARE(graph.nodeById(gate)->gateLimitUsd, 0.25);
}

QTEST_GUILESS_MAIN(PipelineOrderTest)
#include "tst_pipelineorder.moc"
