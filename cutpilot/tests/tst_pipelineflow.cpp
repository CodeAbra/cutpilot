#include <QtTest/QtTest>

#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"

using namespace cutpilot;

namespace {

constexpr auto kBaseModel = "local/procedural-v1";
constexpr auto kEditModel = "local/procedural-edit-v1";
constexpr auto kUpscaleModel = "local/procedural-upscale-v1";
constexpr auto kVendorModel = "openai/gpt-image-1";

int addPrompt(core::NodeGraph &graph, const QString &text)
{
    core::Node node;
    node.kind = core::NodeKind::Prompt;
    node.title = QStringLiteral("Prompt");
    node.promptText = text;
    node.worldSize = QSizeF(260, 170);
    node.ports = { { QStringLiteral("text"), core::PortType::Text, false, 0.5 } };
    return graph.addNode(node);
}

int addGenerate(core::NodeGraph &graph, const QString &modelId = QString())
{
    core::Node node;
    node.kind = core::NodeKind::Generate;
    node.title = QStringLiteral("Generate Image");
    node.modelId = modelId;
    node.worldSize = QSizeF(280, 200);
    node.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
        { QStringLiteral("run"), core::PortType::Control, true, 0.8 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    return graph.addNode(node);
}

int addUpscale(core::NodeGraph &graph)
{
    core::Node node;
    node.kind = core::NodeKind::Generate;
    node.title = QStringLiteral("Upscale Image");
    node.modelId = QLatin1String(kUpscaleModel);
    node.worldSize = QSizeF(240, 160);
    node.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.4 },
        { QStringLiteral("run"), core::PortType::Control, true, 0.7 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    return graph.addNode(node);
}

int addGate(core::NodeGraph &graph, double limitUsd)
{
    core::Node node;
    node.kind = core::NodeKind::CostGate;
    node.title = QStringLiteral("Cost Gate");
    node.gateLimitUsd = limitUsd;
    node.worldSize = QSizeF(200, 130);
    node.ports = {
        { QStringLiteral("run"), core::PortType::Control, true, 0.5 },
        { QStringLiteral("pass"), core::PortType::Control, false, 0.5 },
    };
    return graph.addNode(node);
}

int portIndex(const core::NodeGraph &graph, int nodeId, const QString &name)
{
    const core::Node *node = graph.nodeById(nodeId);
    for (int i = 0; i < node->ports.size(); ++i) {
        if (node->ports[i].name == name)
            return i;
    }
    return -1;
}

int wire(core::NodeGraph &graph, int fromId, const QString &fromPort, int toId,
         const QString &toPort)
{
    core::Connection edge;
    edge.fromNodeId = fromId;
    edge.fromPortIndex = portIndex(graph, fromId, fromPort);
    edge.toNodeId = toId;
    edge.toPortIndex = portIndex(graph, toId, toPort);
    return graph.addConnection(edge);
}

// A prompt feeding a generation whose result feeds an upscale.
struct Chain {
    int promptId = 0;
    int generateId = 0;
    int upscaleId = 0;
};

Chain buildChain(core::NodeGraph &graph, const QString &promptText)
{
    Chain chain;
    chain.promptId = addPrompt(graph, promptText);
    chain.generateId = addGenerate(graph);
    chain.upscaleId = addUpscale(graph);
    wire(graph, chain.promptId, QStringLiteral("text"), chain.generateId,
         QStringLiteral("prompt"));
    wire(graph, chain.generateId, QStringLiteral("result"), chain.upscaleId,
         QStringLiteral("image"));
    return chain;
}

} // namespace

class PipelineFlowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void threeNodeChainRunsEndToEnd();
    void unchangedRerunServesCacheEverywhere();
    void paramChangeRerunsOnlyTheDirtySubgraph();
    void runToHereStopsAtTheTarget();
    void runEverythingReachesAllTerminalsConcurrently();
    void cyclesAreRefused();
    void costGateHoldsItsBranchAndResumes();
    void runCapPausesAndResumesAfterARaise();
    void abortSettlesTheBoard();
    void forcedRerunIgnoresCache();
    void missingKeyMidPipelineFailsTheBranch();
    void secondRunIsRefusedWhileActive();
    void deletedResultFileIsACacheMiss();
    void midRunNodeDeletionPrunesTheRun();
    void midRunParamEditDoesNotClobberTheCache();
    void estimatesShowBeforeAndFinalizeAfter();

private:
    // A coordinator ready to run, with the registry loaded.
    void makeReady(ipc::GenerationCoordinator &coordinator);
    void waitRunFinished(ipc::GenerationCoordinator &coordinator, int timeoutMs);

    QTemporaryDir m_genDir;
    ipc::SidecarHost m_host;
    ipc::GenerationClient m_client;
};

void PipelineFlowTest::initTestCase()
{
    // Key lookups must be deterministic in this suite: env-only, with no
    // vendor key set, so the missing-key path is guaranteed missing.
    qputenv("CUTPILOT_DISABLE_KEYCHAIN", "1");
    qunsetenv("OPENAI_API_KEY");
    QVERIFY(m_genDir.isValid());
    qputenv("CUTPILOT_GEN_DIR", m_genDir.path().toUtf8());

    QSignalSpy readySpy(&m_host, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&m_host, &ipc::SidecarHost::failed);
    m_host.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1, 15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));

    m_client.setEndpoint(m_host.port(), m_host.token());
}

void PipelineFlowTest::cleanupTestCase()
{
    m_host.stop();
}

void PipelineFlowTest::makeReady(ipc::GenerationCoordinator &coordinator)
{
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);
    QVERIFY(!coordinator.models().isEmpty());
}

void PipelineFlowTest::waitRunFinished(ipc::GenerationCoordinator &coordinator,
                                       int timeoutMs)
{
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), timeoutMs);
}

void PipelineFlowTest::threeNodeChainRunsEndToEnd()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a chalk cliff at noon"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);

    // Two generation nodes, exactly two vendor calls.
    QCOMPARE(submissions.count(), 2);

    const core::Node *generate = graph.nodeById(chain.generateId);
    const core::Node *upscale = graph.nodeById(chain.upscaleId);
    QCOMPARE(generate->runState, core::RunState::Done);

    // The upscale consumed the generation's result: exactly doubled, and a
    // different image than its input.
    QCOMPARE(generate->resultWidth, 768);
    QCOMPARE(generate->resultHeight, 512);
    QCOMPARE(upscale->resultWidth, 1536);
    QCOMPARE(upscale->resultHeight, 1024);
    QVERIFY(!generate->resultDigest.isEmpty());
    QVERIFY(!upscale->resultDigest.isEmpty());
    QVERIFY(upscale->resultDigest != generate->resultDigest);

    const ipc::RunSummary summary = coordinator.summary();
    QCOMPARE(summary.fresh, 2);
    QCOMPARE(summary.reused, 0);
    QCOMPARE(summary.failed, 0);
    QCOMPARE(summary.percent(), 100);
    QCOMPARE(summary.spentUsd, 0.002 + 0.001);
}

void PipelineFlowTest::unchangedRerunServesCacheEverywhere()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a tide pool macro"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    const QString generateDigest = graph.nodeById(chain.generateId)->resultDigest;
    const QString upscaleDigest = graph.nodeById(chain.upscaleId)->resultDigest;

    // Nothing changed: the whole graph is served from cache, synchronously,
    // with zero new vendor calls and both nodes marked reused.
    coordinator.runGraph();
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QCOMPARE(graph.nodeById(chain.generateId)->statusMessage,
             QStringLiteral("Reused"));
    QCOMPARE(graph.nodeById(chain.upscaleId)->statusMessage,
             QStringLiteral("Reused"));
    QCOMPARE(graph.nodeById(chain.generateId)->resultDigest, generateDigest);
    QCOMPARE(graph.nodeById(chain.upscaleId)->resultDigest, upscaleDigest);

    const ipc::RunSummary summary = coordinator.summary();
    QCOMPARE(summary.reused, 2);
    QCOMPARE(summary.fresh, 0);
    QCOMPARE(summary.spentUsd, 0.0);
}

void PipelineFlowTest::paramChangeRerunsOnlyTheDirtySubgraph()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a foggy pier"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    const QString generateDigest = graph.nodeById(chain.generateId)->resultDigest;

    // A downstream-only change: the tail node becomes an edit with its own
    // prompt. Only the tail re-runs; the upstream generation is reused.
    core::Node *tail = graph.nodeById(chain.upscaleId);
    tail->modelId = QLatin1String(kEditModel);
    tail->promptText = QStringLiteral("a warm dusk grade");
    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 3);
    QCOMPARE(graph.nodeById(chain.generateId)->statusMessage,
             QStringLiteral("Reused"));
    QCOMPARE(graph.nodeById(chain.generateId)->resultDigest, generateDigest);
    // The edit keeps its input's size and derives new pixels.
    QCOMPARE(graph.nodeById(chain.upscaleId)->resultWidth, 768);
    QVERIFY(graph.nodeById(chain.upscaleId)->resultDigest != generateDigest);

    // An upstream change dirties the whole chain: the new prompt reaches the
    // generation, whose new bytes reach the edit.
    graph.nodeById(chain.promptId)->promptText =
        QStringLiteral("a foggy pier at dawn");
    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 5);
    QVERIFY(graph.nodeById(chain.generateId)->resultDigest != generateDigest);
    QCOMPARE(coordinator.summary().fresh, 2);
}

void PipelineFlowTest::runToHereStopsAtTheTarget()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a salt flat mirage"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runTo(chain.generateId);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.generateId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);

    // Only the upstream subgraph ran; the tail was never part of the run.
    QCOMPARE(submissions.count(), 1);
    QCOMPARE(graph.nodeById(chain.upscaleId)->runState, core::RunState::Idle);
    QVERIFY(graph.nodeById(chain.upscaleId)->resultPath.isEmpty());
    QCOMPARE(coordinator.summary().total, 1);
}

void PipelineFlowTest::runEverythingReachesAllTerminalsConcurrently()
{
    core::NodeGraph graph;
    const int promptA = addPrompt(graph, QStringLiteral("an orchard in rain"));
    const int generateA = addGenerate(graph);
    wire(graph, promptA, QStringLiteral("text"), generateA, QStringLiteral("prompt"));
    const int promptB = addPrompt(graph, QStringLiteral("a comet over dunes"));
    const int generateB = addGenerate(graph);
    wire(graph, promptB, QStringLiteral("text"), generateB, QStringLiteral("prompt"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    // Independent branches submit together in the same pass: both are in
    // flight before either finishes.
    QCOMPARE(coordinator.summary().running, 2);

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generateA)->runState,
                              core::RunState::Done, 60000);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generateB)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QVERIFY(graph.nodeById(generateA)->resultDigest
            != graph.nodeById(generateB)->resultDigest);
}

void PipelineFlowTest::cyclesAreRefused()
{
    core::NodeGraph graph;
    const int a = addGenerate(graph, QLatin1String(kBaseModel));
    const int b = addGenerate(graph, QLatin1String(kBaseModel));
    graph.nodeById(a)->promptText = QStringLiteral("first of a loop");
    graph.nodeById(b)->promptText = QStringLiteral("second of a loop");
    wire(graph, a, QStringLiteral("result"), b, QStringLiteral("image"));
    wire(graph, b, QStringLiteral("result"), a, QStringLiteral("image"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);
    QSignalSpy refusals(&coordinator, &ipc::GenerationCoordinator::runRefused);

    coordinator.runGraph();

    QCOMPARE(refusals.count(), 1);
    QVERIFY(refusals.first().first().toString().contains(QStringLiteral("cycle")));
    QVERIFY(!coordinator.runActive());
    QCOMPARE(submissions.count(), 0);
    QCOMPARE(graph.nodeById(a)->runState, core::RunState::Idle);
    QCOMPARE(graph.nodeById(b)->runState, core::RunState::Idle);
}

void PipelineFlowTest::costGateHoldsItsBranchAndResumes()
{
    core::NodeGraph graph;
    const int prompt = addPrompt(graph, QStringLiteral("a split-branch study"));
    const int freeGenerate = addGenerate(graph);
    const int gatedGenerate = addGenerate(graph);
    wire(graph, prompt, QStringLiteral("text"), freeGenerate,
         QStringLiteral("prompt"));
    wire(graph, prompt, QStringLiteral("text"), gatedGenerate,
         QStringLiteral("prompt"));
    const int gate = addGate(graph, 0.001); // below one generation's price
    wire(graph, gate, QStringLiteral("pass"), gatedGenerate, QStringLiteral("run"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();

    // The gate holds its branch immediately; the free branch runs. The
    // submission acknowledgement arrives with the event loop.
    QCOMPARE(graph.nodeById(gatedGenerate)->runState, core::RunState::Held);
    QVERIFY(graph.nodeById(gatedGenerate)
                ->statusMessage.contains(QStringLiteral("cost gate")));
    QTRY_COMPARE_WITH_TIMEOUT(submissions.count(), 1, 10000);

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(freeGenerate)->runState,
                              core::RunState::Done, 60000);
    QVERIFY(coordinator.runActive());
    ipc::RunSummary summary = coordinator.summary();
    QVERIFY(summary.paused);
    QCOMPARE(summary.held, 1);
    QVERIFY(summary.pauseReason.contains(QStringLiteral("gate")));
    QVERIFY(graph.nodeById(gate)->statusMessage.contains(
        QStringLiteral("Holding")));

    // Raising the limit and resuming releases exactly the held branch.
    graph.nodeById(gate)->gateLimitUsd = 0.05;
    coordinator.resumeRun();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(gatedGenerate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QCOMPARE(graph.nodeById(gate)->gateSpentUsd, 0.002);
    QVERIFY(!coordinator.summary().paused);
}

void PipelineFlowTest::runCapPausesAndResumesAfterARaise()
{
    core::NodeGraph graph;
    const int prompt = addPrompt(graph, QStringLiteral("a capped pair"));
    const int first = addGenerate(graph);
    const int second = addGenerate(graph);
    wire(graph, prompt, QStringLiteral("text"), first, QStringLiteral("prompt"));
    wire(graph, prompt, QStringLiteral("text"), second, QStringLiteral("prompt"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    // Room for one $0.002 job but not two: the second is held before it is
    // ever submitted.
    coordinator.setRunCapUsd(0.003);
    coordinator.runGraph();

    QCOMPARE(graph.nodeById(second)->runState, core::RunState::Held);
    QVERIFY(graph.nodeById(second)->statusMessage.contains(
        QStringLiteral("run cap")));
    QTRY_COMPARE_WITH_TIMEOUT(submissions.count(), 1, 10000);
    ipc::RunSummary summary = coordinator.summary();
    QVERIFY(summary.paused);
    QVERIFY(summary.pauseReason.contains(QStringLiteral("cap")));

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(first)->runState,
                              core::RunState::Done, 60000);
    QVERIFY(coordinator.runActive());

    // Raising the cap and resuming lets the held half through.
    coordinator.setRunCapUsd(0.01);
    coordinator.resumeRun();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(second)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QCOMPARE(coordinator.summary().spentUsd, 0.004);
}

void PipelineFlowTest::abortSettlesTheBoard()
{
    core::NodeGraph graph;
    const int prompt = addPrompt(graph, QStringLiteral("an aborted pair"));
    const int first = addGenerate(graph);
    const int second = addGenerate(graph);
    wire(graph, prompt, QStringLiteral("text"), first, QStringLiteral("prompt"));
    wire(graph, prompt, QStringLiteral("text"), second, QStringLiteral("prompt"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.setRunCapUsd(0.003);
    coordinator.runGraph();
    QCOMPARE(graph.nodeById(second)->runState, core::RunState::Held);
    QTRY_COMPARE_WITH_TIMEOUT(submissions.count(), 1, 10000);

    // Aborting deactivates the run, cancels the in-flight half, and settles
    // the held half back to idle with nothing left claiming to run.
    coordinator.abortRun();
    QVERIFY(!coordinator.runActive());
    QCOMPARE(graph.nodeById(second)->runState, core::RunState::Idle);
    QVERIFY(graph.nodeById(second)->statusMessage.isEmpty());
    // The cancellation races the job's own completion; either terminal
    // outcome leaves a settled board.
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.nodeById(first)->runState == core::RunState::Idle
            || graph.nodeById(first)->runState == core::RunState::Done,
        60000);
    QCOMPARE(submissions.count(), 1);
}

void PipelineFlowTest::forcedRerunIgnoresCache()
{
    core::NodeGraph graph;
    const int prompt = addPrompt(graph, QStringLiteral("a repeatable frame"));
    const int generate = addGenerate(graph);
    wire(graph, prompt, QStringLiteral("text"), generate, QStringLiteral("prompt"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 1);
    const QString digest = graph.nodeById(generate)->resultDigest;

    // The forced re-run really regenerates — and reproduces the same bytes,
    // proving the cache it skipped was valid.
    coordinator.rerunNode(generate);
    QVERIFY(coordinator.runActive());
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QCOMPARE(graph.nodeById(generate)->resultDigest, digest);
}

void PipelineFlowTest::missingKeyMidPipelineFailsTheBranch()
{
    core::NodeGraph graph;
    Chain chain = buildChain(graph, QStringLiteral("a keyless vendor call"));
    graph.nodeById(chain.generateId)->modelId = QLatin1String(kVendorModel);

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);
    QSignalSpy keySpy(&coordinator, &ipc::GenerationCoordinator::addKeyNeeded);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.generateId)->runState,
                              core::RunState::NeedsKey, 15000);
    waitRunFinished(coordinator, 10000);

    // The refusal happened before any job existed, and the node downstream
    // of the failure was skipped, not attempted.
    QCOMPARE(submissions.count(), 0);
    QCOMPARE(keySpy.count(), 1);
    QVERIFY(graph.nodeById(chain.upscaleId)
                ->statusMessage.contains(QStringLiteral("Skipped")));
    QCOMPARE(coordinator.summary().failed, 2);
}

void PipelineFlowTest::secondRunIsRefusedWhileActive()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a busy coordinator"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);
    QSignalSpy refusals(&coordinator, &ipc::GenerationCoordinator::runRefused);

    coordinator.runGraph();
    QVERIFY(coordinator.runActive());
    coordinator.runGraph();
    QCOMPARE(refusals.count(), 1);
    QVERIFY(refusals.first().first().toString().contains(
        QStringLiteral("already active")));

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
}

void PipelineFlowTest::deletedResultFileIsACacheMiss()
{
    core::NodeGraph graph;
    const int prompt = addPrompt(graph, QStringLiteral("an evicted result"));
    const int generate = addGenerate(graph);
    wire(graph, prompt, QStringLiteral("text"), generate, QStringLiteral("prompt"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 1);

    // A cache entry whose file is gone must not be served.
    QVERIFY(QFile::remove(graph.nodeById(generate)->resultPath));
    coordinator.runGraph();
    QVERIFY(coordinator.runActive());
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QVERIFY(QFile::exists(graph.nodeById(generate)->resultPath));
}

void PipelineFlowTest::midRunNodeDeletionPrunesTheRun()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a shrinking board"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.generateId)->runState,
                              core::RunState::Running, 30000);

    // The pending tail disappears mid-run, the way a canvas delete would
    // announce it.
    for (int connectionId : graph.connectionIdsForNode(chain.upscaleId))
        graph.removeConnection(connectionId);
    graph.removeNode(chain.upscaleId);
    coordinator.reconcile();

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.generateId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 1);
    QCOMPARE(coordinator.summary().total, 1);
}

void PipelineFlowTest::midRunParamEditDoesNotClobberTheCache()
{
    core::NodeGraph graph;
    const int prompt = addPrompt(graph, QStringLiteral("the first draft"));
    const int generate = addGenerate(graph);
    wire(graph, prompt, QStringLiteral("text"), generate, QStringLiteral("prompt"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);
    QSignalSpy submissions(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Running, 30000);
    // The prompt changes while the job is in flight: the finished result is
    // stored under what was actually submitted, not the new text.
    graph.nodeById(prompt)->promptText = QStringLiteral("the second draft");
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 1);

    // The new text is dirty and re-runs.
    coordinator.runGraph();
    QVERIFY(coordinator.runActive());
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(generate)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);

    // Reverting the text finds the first result still cached.
    graph.nodeById(prompt)->promptText = QStringLiteral("the first draft");
    coordinator.runGraph();
    waitRunFinished(coordinator, 10000);
    QCOMPARE(submissions.count(), 2);
    QCOMPARE(graph.nodeById(generate)->statusMessage, QStringLiteral("Reused"));
}

void PipelineFlowTest::estimatesShowBeforeAndFinalizeAfter()
{
    core::NodeGraph graph;
    const Chain chain = buildChain(graph, QStringLiteral("a budgeted frame"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    makeReady(coordinator);

    // Registry prices land as estimates before anything runs.
    QCOMPARE(graph.nodeById(chain.generateId)->estimatedCostUsd, 0.002);
    QCOMPARE(graph.nodeById(chain.upscaleId)->estimatedCostUsd, 0.001);
    QCOMPARE(graph.nodeById(chain.generateId)->costUsd, -1.0);

    coordinator.runGraph();
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(chain.upscaleId)->runState,
                              core::RunState::Done, 60000);
    waitRunFinished(coordinator, 10000);

    // The final costs land on the nodes and add up in the summary.
    QCOMPARE(graph.nodeById(chain.generateId)->costUsd, 0.002);
    QCOMPARE(graph.nodeById(chain.upscaleId)->costUsd, 0.001);
    QCOMPARE(coordinator.summary().spentUsd, 0.003);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    PipelineFlowTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_pipelineflow.moc"
