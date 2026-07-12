#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"

using namespace cutpilot;

namespace {

// A prompt node wired into a generate node: the smallest running pipeline.
struct Pipeline {
    int promptId = 0;
    int generateId = 0;
};

Pipeline buildPipeline(core::NodeGraph &graph, const QString &promptText)
{
    core::Node prompt;
    prompt.kind = core::NodeKind::Prompt;
    prompt.title = QStringLiteral("Prompt");
    prompt.promptText = promptText;
    prompt.worldPos = QPointF(0, 0);
    prompt.worldSize = QSizeF(260, 170);
    prompt.ports = { { QStringLiteral("text"), core::PortType::Text, false, 0.5 } };

    core::Node generate;
    generate.kind = core::NodeKind::Generate;
    generate.title = QStringLiteral("Generate Image");
    generate.worldPos = QPointF(500, 0);
    generate.worldSize = QSizeF(280, 200);
    generate.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
        { QStringLiteral("run"), core::PortType::Control, true, 0.8 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };

    Pipeline pipeline;
    pipeline.promptId = graph.addNode(prompt);
    pipeline.generateId = graph.addNode(generate);

    core::Connection wire;
    wire.fromNodeId = pipeline.promptId;
    wire.fromPortIndex = 0;
    wire.toNodeId = pipeline.generateId;
    wire.toPortIndex = 1;
    graph.addConnection(wire);
    return pipeline;
}

QByteArray fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

} // namespace

class GenerationFlowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void runsAPromptThroughTheServiceToADoneNode();
    void unchangedRerunServesTheCachedResult();
    void stopCancelsAnInFlightRun();
    void missingVendorKeySurfacesAddAKey();
    void emptyPromptRefusesTheRunWithoutSubmitting();
    void connectorPulseFollowsInFlightRuns();

private:
    QTemporaryDir m_genDir;
    ipc::SidecarHost m_host;
    ipc::GenerationClient m_client;
};

void GenerationFlowTest::initTestCase()
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

void GenerationFlowTest::cleanupTestCase()
{
    m_host.stop();
}

void GenerationFlowTest::runsAPromptThroughTheServiceToADoneNode()
{
    core::NodeGraph graph;
    const Pipeline pipeline =
        buildPipeline(graph, QStringLiteral("a lighthouse at dusk"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);
    QVERIFY(!coordinator.models().isEmpty());

    // The registry's default model lands on the model-less generate node.
    const core::Node *node = graph.nodeById(pipeline.generateId);
    QCOMPARE(node->modelId, QStringLiteral("local/procedural-v1"));
    QVERIFY(!node->modelLabel.isEmpty());

    // Record every state and progress value the node passes through.
    QVector<core::RunState> states;
    QVector<qreal> progress;
    connect(&coordinator, &ipc::GenerationCoordinator::nodeContentChanged, this,
            [&](int nodeId) {
                const core::Node *changed = graph.nodeById(nodeId);
                if (!changed || nodeId != pipeline.generateId)
                    return;
                if (states.isEmpty() || states.last() != changed->runState)
                    states.append(changed->runState);
                if (changed->runState == core::RunState::Running)
                    progress.append(changed->runProgress);
            });
    QSignalSpy mediaSpy(&coordinator, &ipc::GenerationCoordinator::nodeMediaReady);

    coordinator.runNode(pipeline.generateId);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(pipeline.generateId)->runState,
                              core::RunState::Done, 30000);

    // The run passed through the whole lifecycle, in order.
    QCOMPARE(states.first(), core::RunState::Queued);
    QVERIFY(states.contains(core::RunState::Running));
    QCOMPARE(states.last(), core::RunState::Done);
    QVERIFY(progress.size() >= 2);
    QVERIFY(std::is_sorted(progress.cbegin(), progress.cend()));

    // The node carries the real result: file, cost, resolution.
    node = graph.nodeById(pipeline.generateId);
    QVERIFY(!node->resultPath.isEmpty());
    QVERIFY(QFile::exists(node->resultPath));
    QCOMPARE(node->costUsd, 0.002);
    QCOMPARE(node->resultWidth, 768);
    QCOMPARE(node->resultHeight, 512);

    const QImage result(node->resultPath);
    QVERIFY(!result.isNull());
    QCOMPARE(result.size(), QSize(768, 512));

    // The decoded media arrives off the GUI thread and is delivered for display.
    QTRY_COMPARE_WITH_TIMEOUT(mediaSpy.count(), 1, 10000);
    QCOMPARE(mediaSpy.first().at(0).toInt(), pipeline.generateId);
    QVERIFY(!mediaSpy.first().at(1).value<QImage>().isNull());
}

void GenerationFlowTest::unchangedRerunServesTheCachedResult()
{
    core::NodeGraph graph;
    const Pipeline pipeline =
        buildPipeline(graph, QStringLiteral("a lighthouse at dusk"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);

    coordinator.runNode(pipeline.generateId);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(pipeline.generateId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 10000);
    QCOMPARE(submissionSpy.count(), 1);
    const QByteArray first = fileDigest(graph.nodeById(pipeline.generateId)->resultPath);
    QVERIFY(!first.isEmpty());

    // Nothing changed, so the second run is a cache hit: no vendor call, no
    // Running transition, the identical result marked as reused.
    coordinator.runNode(pipeline.generateId);
    const core::Node *node = graph.nodeById(pipeline.generateId);
    QCOMPARE(node->runState, core::RunState::Done);
    QCOMPARE(node->statusMessage, QStringLiteral("Reused"));
    QCOMPARE(submissionSpy.count(), 1);
    QCOMPARE(fileDigest(node->resultPath), first);
    QVERIFY(!coordinator.runActive());
}

void GenerationFlowTest::stopCancelsAnInFlightRun()
{
    core::NodeGraph graph;
    const Pipeline pipeline =
        buildPipeline(graph, QStringLiteral("a slow-burning skyline"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    coordinator.runNode(pipeline.generateId);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(pipeline.generateId)->runState,
                              core::RunState::Running, 15000);
    coordinator.stopNode(pipeline.generateId);

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(pipeline.generateId)->runState,
                              core::RunState::Idle, 15000);
    const core::Node *node = graph.nodeById(pipeline.generateId);
    QCOMPARE(node->statusMessage, QStringLiteral("Stopped"));
    QVERIFY(node->resultPath.isEmpty()); // no completed result to keep yet
}

void GenerationFlowTest::missingVendorKeySurfacesAddAKey()
{
    core::NodeGraph graph;
    const Pipeline pipeline =
        buildPipeline(graph, QStringLiteral("a keyless experiment"));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    core::Node *node = graph.nodeById(pipeline.generateId);
    node->modelId = QStringLiteral("openai/gpt-image-1");
    node->modelLabel = QStringLiteral("GPT Image 1");

    QSignalSpy keySpy(&coordinator, &ipc::GenerationCoordinator::addKeyNeeded);
    coordinator.runNode(pipeline.generateId);

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(pipeline.generateId)->runState,
                              core::RunState::NeedsKey, 10000);
    QCOMPARE(keySpy.count(), 1);
    QCOMPARE(keySpy.first().at(0).toInt(), pipeline.generateId);
    QCOMPARE(keySpy.first().at(1).toString(), QStringLiteral("openai"));
    QVERIFY(graph.nodeById(pipeline.generateId)
                ->statusMessage.contains(QStringLiteral("Add a key")));
}

void GenerationFlowTest::emptyPromptRefusesTheRunWithoutSubmitting()
{
    core::NodeGraph graph;

    core::Node generate;
    generate.kind = core::NodeKind::Generate;
    generate.title = QStringLiteral("Generate Image");
    generate.worldSize = QSizeF(280, 200);
    generate.ports = {
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.5 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    const int id = graph.addNode(generate);

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    coordinator.runNode(id);

    // The refusal is immediate and local; nothing was submitted.
    QCOMPARE(graph.nodeById(id)->runState, core::RunState::Error);
    QCOMPARE(graph.nodeById(id)->statusMessage, QStringLiteral("Add a prompt"));
}

void GenerationFlowTest::connectorPulseFollowsInFlightRuns()
{
    render::CanvasController controller;
    render::NodeLayerItem layer;
    layer.setSize(QSizeF(1600, 1000));
    layer.setController(&controller);

    core::Node generate;
    generate.kind = core::NodeKind::Generate;
    generate.title = QStringLiteral("Generate Image");
    generate.worldPos = QPointF(200, 200);
    generate.worldSize = QSizeF(280, 200);
    const int id = layer.graph().addNode(generate);

    QVERIFY(!layer.generationPulseActive());

    layer.graph().nodeById(id)->runState = core::RunState::Running;
    layer.refreshNode(id);
    QVERIFY(layer.generationPulseActive());

    layer.graph().nodeById(id)->runState = core::RunState::Done;
    layer.refreshNode(id);
    QVERIFY(!layer.generationPulseActive());
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    GenerationFlowTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_generationflow.moc"
