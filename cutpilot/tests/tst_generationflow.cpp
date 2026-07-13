#include <QtTest/QtTest>

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
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

// A reference still wired into an image-consuming edit model, ready to run.
struct ReferenceRig {
    int stillId = -1;
    int editId = -1;
};

ReferenceRig buildReferenceRig(core::NodeGraph &graph, const QString &refPath)
{
    ReferenceRig rig;

    core::Node still;
    still.kind = core::NodeKind::Still;
    still.title = QStringLiteral("Still Image");
    still.mediaPath = refPath;
    still.worldSize = QSizeF(260, 180);
    still.ports = { { QStringLiteral("image"), core::PortType::Image, false,
                      0.5 } };
    rig.stillId = graph.addNode(still);

    core::Node edit;
    edit.kind = core::NodeKind::Generate;
    edit.title = QStringLiteral("Edit Image");
    edit.promptText = QStringLiteral("weathered postcard grain");
    edit.modelId = QStringLiteral("local/procedural-edit-v1");
    edit.modelLabel = QStringLiteral("Procedural Edit (local)");
    edit.worldPos = QPointF(500, 0);
    edit.worldSize = QSizeF(280, 200);
    edit.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    rig.editId = graph.addNode(edit);

    core::Connection wire;
    wire.fromNodeId = rig.stillId;
    wire.fromPortIndex = 0;
    wire.toNodeId = rig.editId;
    wire.toPortIndex = 0;
    graph.addConnection(wire);
    return rig;
}

bool pinFileClock(const QString &path, const QDateTime &stamp)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.setFileTime(stamp, QFileDevice::FileModificationTime);
}

quint32 crc32Png(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// The PNG with a text chunk carrying the marker inserted before IEND: the
// pixels stay identical, so variants of the same base decode alike while
// their file bytes — what the input digest hashes — differ at equal length.
QByteArray pngWithComment(const QByteArray &basePng, const QByteArray &marker)
{
    const int iendOffset = basePng.size() - 12;
    if (basePng.mid(iendOffset + 4, 4) != QByteArrayLiteral("IEND"))
        return QByteArray();
    const QByteArray body =
        QByteArrayLiteral("tEXt") + QByteArrayLiteral("swap\x00") + marker;
    QByteArray chunk;
    QDataStream stream(&chunk, QIODevice::WriteOnly);
    stream << quint32(body.size() - 4) // payload length excludes the kind
           << quint32(0);              // placeholder, replaced by the body+crc
    chunk = chunk.left(4) + body;
    QByteArray crc;
    QDataStream crcStream(&crc, QIODevice::WriteOnly);
    crcStream << crc32Png(body);
    chunk += crc;
    QByteArray out = basePng;
    out.insert(iendOffset, chunk);
    return out;
}

} // namespace

class GenerationFlowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void runsAPromptThroughTheServiceToADoneNode();
    void referenceImageFeedsAndKeysTheGeneration();
    void rewrittenReferenceWithForgedTimestampRegenerates();
    void largeReferenceHashesOffTheRunThread();
    void vanishedReferenceFileRefusesTheRunWithGuidance();
    void compositeWiredInputRefusesWithTheHonestReason();
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

void GenerationFlowTest::referenceImageFeedsAndKeysTheGeneration()
{
    // A reference still wired into an image-consuming model: the picked file
    // travels into the job, keys the cache, and swapping the file on disk
    // re-generates instead of serving the stale result.
    QTemporaryDir refDir;
    QVERIFY(refDir.isValid());
    const QString refPath = refDir.path() + QStringLiteral("/reference.png");
    QImage(QSize(8, 8), QImage::Format_RGB32).save(refPath);

    core::NodeGraph graph;
    core::Node still;
    still.kind = core::NodeKind::Still;
    still.title = QStringLiteral("Still Image");
    still.mediaPath = refPath;
    still.worldSize = QSizeF(260, 180);
    still.ports = { { QStringLiteral("image"), core::PortType::Image, false,
                      0.5 } };
    const int stillId = graph.addNode(still);

    core::Node edit;
    edit.kind = core::NodeKind::Generate;
    edit.title = QStringLiteral("Edit Image");
    edit.promptText = QStringLiteral("weathered postcard grain");
    edit.modelId = QStringLiteral("local/procedural-edit-v1");
    edit.modelLabel = QStringLiteral("Procedural Edit (local)");
    edit.worldPos = QPointF(500, 0);
    edit.worldSize = QSizeF(280, 200);
    edit.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    const int editId = graph.addNode(edit);

    core::Connection wire;
    wire.fromNodeId = stillId;
    wire.fromPortIndex = 0;
    wire.toNodeId = editId;
    wire.toPortIndex = 0;
    QVERIFY(graph.addConnection(wire) != -1);

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);
    coordinator.runNode(editId);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(editId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 10000);
    QCOMPARE(submissionSpy.count(), 1);
    const QByteArray fromFirstReference =
        fileDigest(graph.nodeById(editId)->resultPath);
    QVERIFY(!fromFirstReference.isEmpty());

    // Unchanged reference, unchanged everything: served from cache.
    coordinator.runNode(editId);
    QCOMPARE(graph.nodeById(editId)->statusMessage, QStringLiteral("Reused"));
    QCOMPARE(submissionSpy.count(), 1);
    QVERIFY(!coordinator.runActive());

    // A different reference is a different generation: the swap re-submits
    // and the produced image genuinely derives from the new pixels.
    QImage swapped(QSize(16, 16), QImage::Format_RGB32);
    swapped.fill(Qt::blue);
    QVERIFY(swapped.save(refPath));
    coordinator.runNode(editId);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 30000);
    QCOMPARE(graph.nodeById(editId)->runState, core::RunState::Done);
    QCOMPARE(submissionSpy.count(), 2);
    QVERIFY(fileDigest(graph.nodeById(editId)->resultPath)
            != fromFirstReference);

    // An image-consuming model with nothing wired in refuses locally with
    // guidance instead of submitting.
    core::NodeGraph bare;
    core::Node lone = edit;
    const int loneId = bare.addNode(lone);
    ipc::GenerationCoordinator loneCoordinator(&bare, &m_client);
    QSignalSpy loneModels(&loneCoordinator,
                          &ipc::GenerationCoordinator::modelsReady);
    loneCoordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(loneModels.count(), 1, 10000);
    loneCoordinator.runNode(loneId);
    QCOMPARE(bare.nodeById(loneId)->runState, core::RunState::Error);
    QCOMPARE(bare.nodeById(loneId)->statusMessage,
             QStringLiteral("Connect an image input"));
}

void GenerationFlowTest::rewrittenReferenceWithForgedTimestampRegenerates()
{
    // The hostile swap: the reference is rewritten with different pixels but
    // the same byte size and the same modification time (a tool preserving
    // timestamps, or two writes inside one clock tick). The changed content
    // must still re-generate — never serve the stale cached result.
    QTemporaryDir refDir;
    QVERIFY(refDir.isValid());
    const QString refPath = refDir.path() + QStringLiteral("/reference.png");
    QByteArray basePng;
    {
        QImage base(QSize(8, 8), QImage::Format_RGB32);
        base.fill(Qt::red);
        QBuffer buffer(&basePng);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(base.save(&buffer, "PNG"));
    }
    const QByteArray firstBytes =
        pngWithComment(basePng, QByteArrayLiteral("aaaa"));
    const QByteArray secondBytes =
        pngWithComment(basePng, QByteArrayLiteral("bbbb"));
    QVERIFY(!firstBytes.isEmpty());
    QCOMPARE(secondBytes.size(), firstBytes.size());
    QVERIFY(secondBytes != firstBytes);

    {
        QFile file(refPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(firstBytes), qint64(firstBytes.size()));
    }
    const qint64 firstSize = QFileInfo(refPath).size();
    const QDateTime pinned = QDateTime::currentDateTime().addSecs(-3600);
    QVERIFY(pinFileClock(refPath, pinned));

    core::NodeGraph graph;
    const ReferenceRig rig = buildReferenceRig(graph, refPath);

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);
    coordinator.runNode(rig.editId);
    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(rig.editId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 10000);
    QCOMPARE(submissionSpy.count(), 1);

    // Rewrite: same size, same forged modification time, different pixels.
    {
        QFile file(refPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(secondBytes), qint64(secondBytes.size()));
    }
    QVERIFY(pinFileClock(refPath, pinned));
    QCOMPARE(QFileInfo(refPath).size(), firstSize);

    coordinator.runNode(rig.editId);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 30000);
    QCOMPARE(graph.nodeById(rig.editId)->runState, core::RunState::Done);
    QVERIFY(graph.nodeById(rig.editId)->statusMessage
            != QStringLiteral("Reused"));
    QCOMPARE(submissionSpy.count(), 2);
}

void GenerationFlowTest::largeReferenceHashesOffTheRunThread()
{
    // A reference too large to hash within a frame's budget must not stall
    // the run decision: starting the run returns with the node still idle,
    // the digest arrives through the event loop, and the run then proceeds
    // to a result that keys the cache exactly like a small reference.
    QTemporaryDir refDir;
    QVERIFY(refDir.isValid());
    const QString refPath = refDir.path() + QStringLiteral("/reference.png");
    {
        // Pseudo-random pixels compress poorly, so the PNG lands well past
        // the inline-hash budget while still decoding as a real image.
        QImage noise(QSize(1024, 1024), QImage::Format_RGB32);
        quint32 state = 0x2545F491u;
        for (int y = 0; y < noise.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                row[x] = qRgb(int((state >> 8) & 0xFF),
                              int((state >> 16) & 0xFF),
                              int((state >> 24) & 0xFF));
            }
        }
        QVERIFY(noise.save(refPath));
    }
    QVERIFY(QFileInfo(refPath).size() > qint64(2) * 1024 * 1024);
    // An old write: once hashed, the file's clocks are trustworthy, so the
    // second run below may serve the cache without waiting again.
    QVERIFY(pinFileClock(refPath,
                         QDateTime::currentDateTime().addSecs(-3600)));

    core::NodeGraph graph;
    const ReferenceRig rig = buildReferenceRig(graph, refPath);

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);
    coordinator.runNode(rig.editId);

    // The decision returned without hashing inline: the run is live, but the
    // node was neither queued nor refused before the event loop turned.
    QVERIFY(coordinator.runActive());
    QCOMPARE(graph.nodeById(rig.editId)->runState, core::RunState::Idle);
    QCOMPARE(submissionSpy.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(graph.nodeById(rig.editId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 10000);
    QCOMPARE(submissionSpy.count(), 1);

    // The off-thread digest keys the cache: an unchanged re-run reuses.
    coordinator.runNode(rig.editId);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator.runActive(), 10000);
    QCOMPARE(graph.nodeById(rig.editId)->statusMessage,
             QStringLiteral("Reused"));
    QCOMPARE(submissionSpy.count(), 1);
}

void GenerationFlowTest::vanishedReferenceFileRefusesTheRunWithGuidance()
{
    // A wired reference whose file was moved or deleted outside the app:
    // the run refuses locally with guidance instead of submitting a job
    // that names a file nobody can read.
    QTemporaryDir refDir;
    QVERIFY(refDir.isValid());
    const QString refPath = refDir.path() + QStringLiteral("/reference.png");
    QImage image(QSize(8, 8), QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(refPath));

    core::NodeGraph graph;
    const ReferenceRig rig = buildReferenceRig(graph, refPath);
    QVERIFY(QFile::remove(refPath));

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);
    coordinator.runNode(rig.editId);
    QCOMPARE(graph.nodeById(rig.editId)->runState, core::RunState::Error);
    QCOMPARE(graph.nodeById(rig.editId)->statusMessage,
             QStringLiteral("Reference file missing"));
    QCOMPARE(submissionSpy.count(), 0);
    QVERIFY(!coordinator.runActive());
}

void GenerationFlowTest::compositeWiredInputRefusesWithTheHonestReason()
{
    // A compositing node's output wires into an image input under the port
    // rules, but composites render textures, not files, so nothing can feed
    // the job. The refusal must say that — not pretend nothing is wired.
    core::NodeGraph graph;
    core::Node blend;
    blend.kind = core::NodeKind::Blend;
    blend.title = QStringLiteral("Blend");
    blend.worldSize = QSizeF(260, 180);
    blend.ports = {
        { QStringLiteral("base"), core::PortType::Image, true, 0.35 },
        { QStringLiteral("over"), core::PortType::Image, true, 0.65 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    const int blendId = graph.addNode(blend);

    core::Node edit;
    edit.kind = core::NodeKind::Generate;
    edit.title = QStringLiteral("Edit Image");
    edit.promptText = QStringLiteral("weathered postcard grain");
    edit.modelId = QStringLiteral("local/procedural-edit-v1");
    edit.modelLabel = QStringLiteral("Procedural Edit (local)");
    edit.worldPos = QPointF(500, 0);
    edit.worldSize = QSizeF(280, 200);
    edit.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    const int editId = graph.addNode(edit);

    core::Connection wire;
    wire.fromNodeId = blendId;
    wire.fromPortIndex = 2;
    wire.toNodeId = editId;
    wire.toPortIndex = 0;
    QVERIFY(graph.addConnection(wire) != -1);

    ipc::GenerationCoordinator coordinator(&graph, &m_client);
    QSignalSpy modelsSpy(&coordinator, &ipc::GenerationCoordinator::modelsReady);
    coordinator.serviceBecameReady();
    QTRY_COMPARE_WITH_TIMEOUT(modelsSpy.count(), 1, 10000);

    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);
    coordinator.runNode(editId);
    QCOMPARE(graph.nodeById(editId)->runState, core::RunState::Error);
    QCOMPARE(graph.nodeById(editId)->statusMessage,
             QStringLiteral("Composite outputs can't feed generations"));
    QCOMPARE(submissionSpy.count(), 0);
    QVERIFY(!coordinator.runActive());
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
