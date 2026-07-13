#include <QtTest/QtTest>

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>

#include "QuickPanel.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

using namespace cutpilot;
using cutpilot::app::QuickPanel;
using cutpilot::app::quickOutputSize;

// The quick surface over the real stack: a live sidecar process, the real
// coordinator and cache, a real canvas layer, and the panel itself — the
// one-prompt flow driven end to end without a pointer.
class QuickModeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void outputSizeMathIsExact();
    void outputFormatSetterHoldsTheServiceBounds();
    void openMaterializesOneRealGenerateNode();
    void adoptionPrefersTheOldestQuickNode();
    void editsLandOnTheNodeThroughTheUndoablePath();
    void runProducesASizedResultAndTheCacheStaysHonest();
    void missingVendorKeySurfacesTheAddKeyAffordance();
    void leavingQuickModeRevealsTheNodeAndAdoptsItBack();

private:
    // One rig per test: a real layer and camera with the panel bound over
    // them, the coordinator already fed by the live registry.
    struct Rig {
        theme::ThemeTable table{ theme::Theme::Dark };
        render::CanvasController controller;
        render::NodeLayerItem layer;
        ipc::GenerationCoordinator coordinator;
        QuickPanel panel;

        explicit Rig(ipc::GenerationClient *client)
            : coordinator(&layer.graph(), client)
            , panel(table, &layer, &coordinator, nullptr)
        {
            layer.setSize(QSizeF(1600, 1000));
            layer.setController(&controller);
            // Mirror the window's wiring: decoded results become the card's
            // media body.
            QObject::connect(&coordinator,
                             &ipc::GenerationCoordinator::nodeMediaReady,
                             &layer,
                             [this](int nodeId, const QImage &image) {
                                 layer.setNodeMedia(nodeId, image);
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

void QuickModeTest::initTestCase()
{
    // Key lookups must be deterministic: env-only, with no vendor key set,
    // so the missing-key path is guaranteed missing.
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

void QuickModeTest::cleanupTestCase()
{
    m_host.stop();
}

void QuickModeTest::outputSizeMathIsExact()
{
    QCOMPARE(quickOutputSize(1080, 1, 1), QSize(1080, 1080));
    QCOMPARE(quickOutputSize(1080, 3, 4), QSize(1080, 1440));
    QCOMPARE(quickOutputSize(1080, 16, 9), QSize(1920, 1080));
    QCOMPARE(quickOutputSize(768, 9, 16), QSize(768, 1366));
    QCOMPARE(quickOutputSize(512, 3, 2), QSize(768, 512));

    // Sides are held inside the service's accepted range, even for extremes.
    QCOMPARE(quickOutputSize(2048, 16, 9), QSize(2048, 2048));
    QCOMPARE(quickOutputSize(8, 1, 1), QSize(64, 64));
}

void QuickModeTest::outputFormatSetterHoldsTheServiceBounds()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));
    rig.panel.openAt(QPointF(0.0, 0.0));
    const int nodeId = rig.panel.nodeId();

    // Sizes land through the undoable setter already inside the generation
    // service's accepted range, however far outside a caller reaches.
    rig.layer.setNodeOutputFormat(nodeId, 9000, 9000);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 2048);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 2048);

    rig.layer.setNodeOutputFormat(nodeId, 16, 16);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 64);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 64);

    // A half-set pair is not a format; the node keeps what it had.
    rig.layer.setNodeOutputFormat(nodeId, 1080, 0);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 64);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 64);

    // Zero for both still means the model's own default.
    rig.layer.setNodeOutputFormat(nodeId, 0, 0);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 0);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 0);
}

void QuickModeTest::openMaterializesOneRealGenerateNode()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));
    QVERIFY(rig.layer.graph().nodes().isEmpty());

    rig.panel.openAt(QPointF(120.0, 80.0));

    // One real generate node, fully typed, carrying the registry default so
    // it can run at once.
    QCOMPARE(rig.layer.graph().nodes().size(), 1);
    const core::Node &node = rig.layer.graph().nodes().first();
    QCOMPARE(rig.panel.nodeId(), node.id);
    QCOMPARE(node.kind, core::NodeKind::Generate);
    QCOMPARE(node.title, QStringLiteral("Quick Generate"));
    QCOMPARE(node.modelId, QStringLiteral("local/procedural-v1"));
    QCOMPARE(node.ports.size(), 4);
    QCOMPARE(node.ports[0].type, core::PortType::Image);
    QVERIFY(node.ports[0].isInput);
    QCOMPARE(node.ports[1].type, core::PortType::Text);
    QVERIFY(node.ports[1].isInput);
    QCOMPARE(node.ports[2].type, core::PortType::Control);
    QCOMPARE(node.ports[3].type, core::PortType::Image);
    QVERIFY(!node.ports[3].isInput);
    QVERIFY(node.containsWorld(QPointF(120.0, 80.0)));

    // The materialization is one undo step; undoing it resets the surface.
    QSignalSpy dismissedSpy(&rig.panel, &QuickPanel::dismissed);
    rig.layer.undo();
    QVERIFY(rig.layer.graph().nodes().isEmpty());
    QCOMPARE(rig.panel.nodeId(), -1);
    QCOMPARE(dismissedSpy.count(), 1);
    QVERIFY(!rig.panel.isVisible());
}

void QuickModeTest::adoptionPrefersTheOldestQuickNode()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));

    // Duplicates can reach the board — a placed template can carry a saved
    // quick node — so adoption must stay with the oldest match instead of
    // jumping to whichever duplicate arrived last.
    core::Node prototype =
        core::catalogPrototype(QStringLiteral("Generate Image"));
    prototype.title = QStringLiteral("Quick Generate");
    const int oldest = rig.layer.placePrototypeAt(prototype, QPointF(0.0, 0.0));
    rig.layer.placePrototypeAt(prototype, QPointF(500.0, 0.0));

    rig.panel.openAt(QPointF(250.0, 250.0));
    QCOMPARE(rig.panel.nodeId(), oldest);
    QCOMPARE(rig.layer.graph().nodes().size(), 2);

    // Re-opening after a dismissal lands on the same node again, even with
    // yet another duplicate in between.
    rig.panel.dismiss();
    rig.layer.placePrototypeAt(prototype, QPointF(0.0, 500.0));
    rig.panel.openAt(QPointF(250.0, 250.0));
    QCOMPARE(rig.panel.nodeId(), oldest);
    QCOMPARE(rig.layer.graph().nodes().size(), 3);
}

void QuickModeTest::editsLandOnTheNodeThroughTheUndoablePath()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));
    rig.panel.openAt(QPointF(0.0, 0.0));
    const int nodeId = rig.panel.nodeId();

    // A chip pick right after typing must carry the typed prompt with it —
    // the field's text lands on the node, never discarded by the sync that
    // follows the format edit.
    auto *prompt = rig.panel.findChild<QPlainTextEdit *>(
        QStringLiteral("quickPrompt"));
    QVERIFY(prompt);
    prompt->setPlainText(QStringLiteral("a granite coast at dawn"));
    rig.panel.applyPreset(1080, 3, 4);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->promptText,
             QStringLiteral("a granite coast at dawn"));
    QCOMPARE(prompt->toPlainText(), QStringLiteral("a granite coast at dawn"));
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 1080);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 1440);

    // A tier change keeps the picked aspect; an aspect change keeps the tier.
    rig.panel.applyTier(768);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 768);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 1024);
    rig.panel.applyAspect(16, 9);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 1366);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 768);

    rig.panel.applyModel(QStringLiteral("local/procedural-edit-v1"),
                         QStringLiteral("Procedural Edit (local)"));
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->modelId,
             QStringLiteral("local/procedural-edit-v1"));

    // Every edit above is a plain history step on the shared stack.
    rig.layer.undo();
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->modelId,
             QStringLiteral("local/procedural-v1"));
    rig.layer.undo();
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputWidth, 768);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->outputHeight, 1024);
}

void QuickModeTest::runProducesASizedResultAndTheCacheStaysHonest()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));
    rig.panel.openAt(QPointF(0.0, 0.0));
    const int nodeId = rig.panel.nodeId();

    auto *prompt = rig.panel.findChild<QPlainTextEdit *>(
        QStringLiteral("quickPrompt"));
    auto *run = rig.panel.findChild<QPushButton *>(QStringLiteral("quickRun"));
    auto *status = rig.panel.findChild<QLabel *>(QStringLiteral("quickStatus"));
    auto *result = rig.panel.findChild<QLabel *>(QStringLiteral("quickResult"));
    QVERIFY(prompt && run && status && result);
    QVERIFY(!result->isVisibleTo(&rig.panel));

    prompt->setPlainText(QStringLiteral("a lighthouse at dusk"));
    rig.panel.applyPreset(512, 1, 1);

    // The run passes through the real lifecycle on the real coordinator.
    QVector<core::RunState> states;
    connect(&rig.coordinator, &ipc::GenerationCoordinator::nodeContentChanged,
            this, [&](int changedId) {
                const core::Node *node = rig.layer.graph().nodeById(changedId);
                if (!node || changedId != nodeId)
                    return;
                if (states.isEmpty() || states.last() != node->runState)
                    states.append(node->runState);
            });
    QSignalSpy submissionSpy(&m_client, &ipc::GenerationClient::jobSubmitted);
    QSignalSpy mediaSpy(&rig.coordinator,
                        &ipc::GenerationCoordinator::nodeMediaReady);

    run->click();
    QTRY_COMPARE_WITH_TIMEOUT(rig.layer.graph().nodeById(nodeId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.coordinator.runActive(), 10000);

    QCOMPARE(states.first(), core::RunState::Queued);
    QVERIFY(states.contains(core::RunState::Running));
    QCOMPARE(states.last(), core::RunState::Done);
    QCOMPARE(submissionSpy.count(), 1);

    // The node carries the real result at the picked format, and the panel
    // mirrors it: the finished image, the size, and the true cost.
    const core::Node *node = rig.layer.graph().nodeById(nodeId);
    QVERIFY(QFile::exists(node->resultPath));
    QCOMPARE(node->resultWidth, 512);
    QCOMPARE(node->resultHeight, 512);
    QCOMPARE(node->costUsd, 0.002);
    const QImage produced(node->resultPath);
    QCOMPARE(produced.size(), QSize(512, 512));

    QTRY_VERIFY_WITH_TIMEOUT(mediaSpy.count() >= 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(result->isVisibleTo(&rig.panel), 10000);
    QVERIFY(!result->pixmap().isNull());
    QVERIFY(status->text().contains(QStringLiteral("Done")));
    QVERIFY(status->text().contains(QStringLiteral("512 × 512")));
    QVERIFY(status->text().contains(QStringLiteral("$0.002")));

    // Unchanged, the second run is served from the cache without a vendor
    // call — and the surface says so.
    run->click();
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->statusMessage,
             QStringLiteral("Reused"));
    QCOMPARE(submissionSpy.count(), 1);
    QVERIFY(status->text().contains(QStringLiteral("Reused")));
    // The cache hit re-delivers the media, so a reused result still reaches
    // the card and the surface — awaited here so the decode never dangles
    // into the next run's assertions.
    QTRY_VERIFY_WITH_TIMEOUT(mediaSpy.count() >= 2, 10000);

    // A new format is a new result: the cache must not serve the old size.
    rig.panel.applyTier(768);
    run->click();
    QTRY_COMPARE_WITH_TIMEOUT(rig.layer.graph().nodeById(nodeId)->resultWidth,
                              768, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.coordinator.runActive(), 10000);
    QCOMPARE(submissionSpy.count(), 2);
    QCOMPARE(rig.layer.graph().nodeById(nodeId)->resultHeight, 768);
}

void QuickModeTest::missingVendorKeySurfacesTheAddKeyAffordance()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));
    rig.panel.openAt(QPointF(0.0, 0.0));
    const int nodeId = rig.panel.nodeId();

    auto *prompt = rig.panel.findChild<QPlainTextEdit *>(
        QStringLiteral("quickPrompt"));
    auto *run = rig.panel.findChild<QPushButton *>(QStringLiteral("quickRun"));
    auto *addKey =
        rig.panel.findChild<QPushButton *>(QStringLiteral("quickAddKey"));
    QVERIFY(prompt && run && addKey);
    QVERIFY(!addKey->isVisibleTo(&rig.panel));

    prompt->setPlainText(QStringLiteral("a keyless experiment"));
    rig.panel.applyModel(QStringLiteral("openai/gpt-image-1"),
                         QStringLiteral("GPT Image 1"));
    run->click();

    QTRY_COMPARE_WITH_TIMEOUT(rig.layer.graph().nodeById(nodeId)->runState,
                              core::RunState::NeedsKey, 10000);
    QVERIFY(addKey->isVisibleTo(&rig.panel));
    QVERIFY(rig.layer.graph()
                .nodeById(nodeId)
                ->statusMessage.contains(QStringLiteral("Add a key")));

    // The affordance asks for the right vendor's key.
    QSignalSpy keySpy(&rig.panel, &QuickPanel::addKeyRequested);
    addKey->click();
    QCOMPARE(keySpy.count(), 1);
    QCOMPARE(keySpy.first().at(0).toInt(), nodeId);
    QCOMPARE(keySpy.first().at(1).toString(), QStringLiteral("openai"));
}

void QuickModeTest::leavingQuickModeRevealsTheNodeAndAdoptsItBack()
{
    Rig rig(&m_client);
    QVERIFY(waitForModels(rig));
    rig.panel.openAt(QPointF(40.0, 40.0));
    const int nodeId = rig.panel.nodeId();

    auto *prompt = rig.panel.findChild<QPlainTextEdit *>(
        QStringLiteral("quickPrompt"));
    auto *run = rig.panel.findChild<QPushButton *>(QStringLiteral("quickRun"));
    QVERIFY(prompt && run);
    prompt->setPlainText(QStringLiteral("one shot, then the full canvas"));
    rig.panel.applyPreset(512, 1, 1);
    run->click();
    QTRY_COMPARE_WITH_TIMEOUT(rig.layer.graph().nodeById(nodeId)->runState,
                              core::RunState::Done, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.layer.nodeMediaImage(nodeId).isNull(), 10000);

    // Leaving quick mode is not an export: the same node stays in the
    // document with its result and its typed ports ready to wire onward.
    rig.panel.dismiss();
    QVERIFY(!rig.panel.isVisible());
    const core::Node *node = rig.layer.graph().nodeById(nodeId);
    QVERIFY(node);
    QCOMPARE(rig.layer.graph().nodes().size(), 1);
    QVERIFY(QFile::exists(node->resultPath));
    QVERIFY(!rig.layer.nodeMediaImage(nodeId).isNull());
    QCOMPARE(node->ports[3].type, core::PortType::Image);
    QVERIFY(!node->ports[3].isInput);

    // Toggling back adopts the same node — never a duplicate — with the
    // prompt and result already in place.
    rig.panel.openAt(QPointF(900.0, 900.0));
    QCOMPARE(rig.panel.nodeId(), nodeId);
    QCOMPARE(rig.layer.graph().nodes().size(), 1);
    QCOMPARE(prompt->toPlainText(),
             QStringLiteral("one shot, then the full canvas"));
    auto *result = rig.panel.findChild<QLabel *>(QStringLiteral("quickResult"));
    QVERIFY(result && result->isVisibleTo(&rig.panel));

    // The user's close control reports the exit so the toggle can follow.
    QSignalSpy dismissedSpy(&rig.panel, &QuickPanel::dismissed);
    auto *close =
        rig.panel.findChild<QToolButton *>(QStringLiteral("quickClose"));
    QVERIFY(close);
    close->click();
    QCOMPARE(dismissedSpy.count(), 1);
    QVERIFY(!rig.panel.isVisible());
}

QTEST_MAIN(QuickModeTest)
#include "tst_quickmode.moc"
