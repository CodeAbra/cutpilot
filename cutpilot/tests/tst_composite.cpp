#include <QtTest/QtTest>

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/CompositePlan.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/PipelineOrder.h"
#include "cutpilot/core/PortRules.h"
#include "cutpilot/core/command/CommandStack.h"
#include "cutpilot/core/command/SetCompositeParamsCommand.h"
#include "cutpilot/core/command/SetMediaPathCommand.h"

#include <memory>

using namespace cutpilot::core;

namespace {

int addProto(NodeGraph &graph, NodeKind kind)
{
    return graph.addNode(compositeNodePrototype(kind));
}

// Wire the source node's output port into the target node's input port,
// verifying the connection is accepted.
void wire(NodeGraph &graph, int from, int fromPort, int to, int toPort)
{
    Connection edge;
    edge.fromNodeId = from;
    edge.fromPortIndex = fromPort;
    edge.toNodeId = to;
    edge.toPortIndex = toPort;
    QVERIFY(graph.addConnection(edge) != -1);
}

int passIndex(const CompositePlan &plan, int nodeId)
{
    for (int i = 0; i < plan.passes.size(); ++i) {
        if (plan.passes[i].nodeId == nodeId)
            return i;
    }
    return -1;
}

} // namespace

class CompositeTest : public QObject {
    Q_OBJECT

private slots:
    void prototypesWireUnderThePortRules();
    void compositeKindsStayOutOfTheGenerationRun();
    void paramsCommandAppliesUndoesAndRedoes();
    void paramsCommandRecordsALiveScrub();
    void mediaPathCommandRoundTrips();
    void planOrdersDependenciesFirst();
    void planCarriesMatteFlagsAndUnwiredInputs();
    void planSharesADiamondSourceOnce();
    void planRefusesACycle();
    void planRejectsANonImageTarget();
    void signatureIsStable();
    void signatureFlipsOnParamAndFollowsTheChain();
    void signatureFollowsRewiringAndSourceIdentity();
    void signatureDistinguishesSourceOutputPorts();
};

void CompositeTest::prototypesWireUnderThePortRules()
{
    // A still feeds a key; the key's matte (a mask output) may feed a mask
    // input directly and an image input only as a conversion.
    const Node key = compositeNodePrototype(NodeKind::Key);
    QCOMPARE(key.ports.size(), 3);
    QCOMPARE(key.ports[2].type, PortType::Mask);
    QVERIFY(!key.ports[2].isInput);

    const Node mask = compositeNodePrototype(NodeKind::Mask);
    QCOMPARE(portMatch(key.ports[2].type, mask.ports[1].type), PortMatch::Direct);

    const Node blend = compositeNodePrototype(NodeKind::Blend);
    QCOMPARE(portMatch(key.ports[2].type, blend.ports[1].type),
             PortMatch::Converted);

    const Node still = compositeNodePrototype(NodeKind::Still);
    QCOMPARE(portMatch(still.ports[0].type, key.ports[0].type), PortMatch::Direct);
}

void CompositeTest::compositeKindsStayOutOfTheGenerationRun()
{
    for (NodeKind kind : { NodeKind::Still, NodeKind::Blend, NodeKind::Mask,
                           NodeKind::Key, NodeKind::Transform }) {
        Node node = compositeNodePrototype(kind);
        QVERIFY(!isEvaluatable(node));
        QVERIFY(producesImage(kind));
    }
    QVERIFY(!isCompositeKind(NodeKind::Still));
    QVERIFY(!isCompositeKind(NodeKind::Generate));
    QVERIFY(isCompositeKind(NodeKind::Blend));
}

void CompositeTest::paramsCommandAppliesUndoesAndRedoes()
{
    NodeGraph graph;
    const int blend = addProto(graph, NodeKind::Blend);

    const CompositeParams before = graph.nodeById(blend)->comp;
    CompositeParams after = before;
    after.blendMode = BlendMode::Multiply;
    after.opacity = 0.4;

    CommandStack stack;
    const int revision = graph.nodeById(blend)->contentRevision;
    stack.push(std::make_unique<SetCompositeParamsCommand>(blend, before, after),
               graph);
    QCOMPARE(graph.nodeById(blend)->comp, after);
    QVERIFY(graph.nodeById(blend)->contentRevision > revision);

    stack.undo(graph);
    QCOMPARE(graph.nodeById(blend)->comp, before);

    stack.redo(graph);
    QCOMPARE(graph.nodeById(blend)->comp, after);
}

void CompositeTest::paramsCommandRecordsALiveScrub()
{
    NodeGraph graph;
    const int transform = addProto(graph, NodeKind::Transform);

    // A scrub writes values directly for live feedback, then records the
    // whole gesture without re-applying it; one undo restores the start.
    const CompositeParams before = graph.nodeById(transform)->comp;
    CompositeParams live = before;
    live.rotationDeg = 12.0;
    live.scale = 1.5;
    graph.nodeById(transform)->comp = live;

    CommandStack stack;
    stack.record(std::make_unique<SetCompositeParamsCommand>(transform, before, live));
    QCOMPARE(graph.nodeById(transform)->comp, live);

    stack.undo(graph);
    QCOMPARE(graph.nodeById(transform)->comp, before);
}

void CompositeTest::mediaPathCommandRoundTrips()
{
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);

    CommandStack stack;
    stack.push(std::make_unique<SetMediaPathCommand>(
                   still, QStringLiteral("/media/first.png")),
               graph);
    QCOMPARE(graph.nodeById(still)->mediaPath, QStringLiteral("/media/first.png"));

    stack.push(std::make_unique<SetMediaPathCommand>(
                   still, QStringLiteral("/media/second.png")),
               graph);
    stack.undo(graph);
    QCOMPARE(graph.nodeById(still)->mediaPath, QStringLiteral("/media/first.png"));
    stack.undo(graph);
    QCOMPARE(graph.nodeById(still)->mediaPath, QString());
}

void CompositeTest::planOrdersDependenciesFirst()
{
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int blend = addProto(graph, NodeKind::Blend);
    wire(graph, still, 0, key, 0);
    wire(graph, key, 1, blend, 1); // keyed image into the over input

    const CompositePlan plan = buildCompositePlan(graph, blend);
    QVERIFY(plan.valid);
    QCOMPARE(plan.targetNodeId, blend);
    QCOMPARE(plan.sourceNodeIds, QVector<int>{ still });
    QCOMPARE(plan.passes.size(), 2);
    QVERIFY(passIndex(plan, key) < passIndex(plan, blend));
    QCOMPARE(plan.passes.last().nodeId, blend);
}

void CompositeTest::planCarriesMatteFlagsAndUnwiredInputs()
{
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int mask = addProto(graph, NodeKind::Mask);
    wire(graph, still, 0, mask, 0);
    wire(graph, still, 0, key, 0);
    wire(graph, key, 2, mask, 1); // the matte output feeds the mask input

    const CompositePlan plan = buildCompositePlan(graph, mask);
    QVERIFY(plan.valid);
    const CompositePass &maskPass = plan.passes[passIndex(plan, mask)];
    QCOMPARE(maskPass.inputs.size(), 2);
    QCOMPARE(maskPass.inputs[0].nodeId, still);
    QVERIFY(!maskPass.inputs[0].matte);
    QCOMPARE(maskPass.inputs[1].nodeId, key);
    QVERIFY(maskPass.inputs[1].matte);

    // A blend with nothing wired still plans, with both inputs unwired.
    const int blend = addProto(graph, NodeKind::Blend);
    const CompositePlan bare = buildCompositePlan(graph, blend);
    QVERIFY(bare.valid);
    QCOMPARE(bare.passes.size(), 1);
    QCOMPARE(bare.passes[0].inputs[0].nodeId, -1);
    QCOMPARE(bare.passes[0].inputs[1].nodeId, -1);
}

void CompositeTest::planSharesADiamondSourceOnce()
{
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int transform = addProto(graph, NodeKind::Transform);
    const int blend = addProto(graph, NodeKind::Blend);
    wire(graph, still, 0, key, 0);
    wire(graph, still, 0, transform, 0);
    wire(graph, key, 1, blend, 0);
    wire(graph, transform, 1, blend, 1);

    const CompositePlan plan = buildCompositePlan(graph, blend);
    QVERIFY(plan.valid);
    QCOMPARE(plan.sourceNodeIds, QVector<int>{ still });
    QCOMPARE(plan.passes.size(), 3);
    QVERIFY(passIndex(plan, key) < passIndex(plan, blend));
    QVERIFY(passIndex(plan, transform) < passIndex(plan, blend));
}

void CompositeTest::planRefusesACycle()
{
    NodeGraph graph;
    const int a = addProto(graph, NodeKind::Blend);
    const int b = addProto(graph, NodeKind::Blend);
    wire(graph, a, 2, b, 0);
    wire(graph, b, 2, a, 0);

    const CompositePlan plan = buildCompositePlan(graph, a);
    QVERIFY(!plan.valid);
    QVERIFY(plan.passes.isEmpty());
}

void CompositeTest::planRejectsANonImageTarget()
{
    NodeGraph graph;
    Node prompt;
    prompt.kind = NodeKind::Prompt;
    prompt.ports = { { QStringLiteral("text"), PortType::Text, false, 0.5 } };
    const int promptId = graph.addNode(prompt);

    QVERIFY(!buildCompositePlan(graph, promptId).valid);
    QVERIFY(!buildCompositePlan(graph, 9999).valid);
}

void CompositeTest::signatureIsStable()
{
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int blend = addProto(graph, NodeKind::Blend);
    wire(graph, still, 0, blend, 0);

    const QString first = compositeSignature(graph, blend);
    const QString second = compositeSignature(graph, blend);
    QCOMPARE(first, second);
    QVERIFY(!first.isEmpty());
}

void CompositeTest::signatureFlipsOnParamAndFollowsTheChain()
{
    NodeGraph graph;
    const int still = addProto(graph, NodeKind::Still);
    const int key = addProto(graph, NodeKind::Key);
    const int blend = addProto(graph, NodeKind::Blend);
    wire(graph, still, 0, key, 0);
    wire(graph, key, 1, blend, 1);

    const QString keyBefore = compositeSignature(graph, key);
    const QString blendBefore = compositeSignature(graph, blend);
    const QString stillBefore = compositeSignature(graph, still);

    // A parameter change on the key flips the key and everything downstream,
    // never the source upstream of it.
    graph.nodeById(key)->comp.keyTolerance = 0.5;
    QVERIFY(compositeSignature(graph, key) != keyBefore);
    QVERIFY(compositeSignature(graph, blend) != blendBefore);
    QCOMPARE(compositeSignature(graph, still), stillBefore);
}

void CompositeTest::signatureFollowsRewiringAndSourceIdentity()
{
    NodeGraph graph;
    const int stillA = addProto(graph, NodeKind::Still);
    const int stillB = addProto(graph, NodeKind::Still);
    graph.nodeById(stillA)->mediaPath = QStringLiteral("/media/a.png");
    graph.nodeById(stillB)->mediaPath = QStringLiteral("/media/b.png");
    const int transform = addProto(graph, NodeKind::Transform);
    wire(graph, stillA, 0, transform, 0);

    const QString onA = compositeSignature(graph, transform);

    // Re-route to the other source: the signature must follow the wiring.
    const int edge = graph.connectionAtInput(transform, 0);
    graph.removeConnection(edge);
    wire(graph, stillB, 0, transform, 0);
    const QString onB = compositeSignature(graph, transform);
    QVERIFY(onA != onB);

    // A generation source's identity is its result digest.
    NodeGraph runGraph;
    Node generate;
    generate.kind = NodeKind::Generate;
    generate.ports = { { QStringLiteral("result"), PortType::Image, false, 0.5 } };
    const int gen = runGraph.addNode(generate);
    const int t2 = addProto(runGraph, NodeKind::Transform);
    wire(runGraph, gen, 0, t2, 0);

    const QString beforeResult = compositeSignature(runGraph, t2);
    runGraph.nodeById(gen)->resultPath = QStringLiteral("/results/x.png");
    runGraph.nodeById(gen)->resultDigest = QStringLiteral("abc123");
    QVERIFY(compositeSignature(runGraph, t2) != beforeResult);
}

void CompositeTest::signatureDistinguishesSourceOutputPorts()
{
    // A node can carry two outputs of the same port type with different
    // content behind them; which output feeds a consumer must shape the
    // consumer's signature, or the cache would serve one output's texture
    // for the other.
    NodeGraph graph;
    Node dual;
    dual.kind = NodeKind::Generate;
    dual.ports = { { QStringLiteral("left"), PortType::Image, false, 0.35 },
                   { QStringLiteral("right"), PortType::Image, false, 0.65 } };
    const int source = graph.addNode(dual);
    const int transform = addProto(graph, NodeKind::Transform);

    wire(graph, source, 0, transform, 0);
    const QString onFirstOutput = compositeSignature(graph, transform);
    CompositePlan plan = buildCompositePlan(graph, transform);
    QCOMPARE(plan.passes.last().inputs[0].fromPortIndex, 0);

    graph.removeConnection(graph.connectionAtInput(transform, 0));
    wire(graph, source, 1, transform, 0);
    QVERIFY(compositeSignature(graph, transform) != onFirstOutput);
    plan = buildCompositePlan(graph, transform);
    QCOMPARE(plan.passes.last().inputs[0].fromPortIndex, 1);
}

QTEST_GUILESS_MAIN(CompositeTest)
#include "tst_composite.moc"
