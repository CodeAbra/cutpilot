#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

#include "WorkflowStore.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"

using namespace cutpilot;
using cutpilot::app::WorkflowStore;

namespace {

core::NodeGraph sampleBoard()
{
    core::NodeGraph graph;
    core::Node prompt = core::catalogPrototype(QStringLiteral("Prompt"));
    prompt.promptText = QStringLiteral("dawn over the harbor");
    const int promptId = graph.addNode(prompt);
    const int generateId =
        graph.addNode(core::catalogPrototype(QStringLiteral("Generate Image")));
    core::Connection wire;
    wire.fromNodeId = promptId;
    wire.fromPortIndex = 0;
    wire.toNodeId = generateId;
    wire.toPortIndex = 1;
    graph.addConnection(wire);
    return graph;
}

} // namespace

class WorkflowStoreTest : public QObject {
    Q_OBJECT

private slots:
    void savesAndLoadsTheDocument()
    {
        QTemporaryDir dir;
        core::NodeGraph graph = sampleBoard();
        WorkflowStore store(&graph);
        store.setDirectory(dir.path());
        store.setName(QStringLiteral("Harbor Reel"));
        QVERIFY(store.saveNow());
        QCOMPARE(store.state(), WorkflowStore::State::Saved);
        QVERIFY(QFile::exists(store.filePath()));

        core::NodeGraph restored;
        WorkflowStore reader(&restored);
        reader.setDirectory(dir.path());
        QVERIFY(reader.load());
        QCOMPARE(reader.name(), QStringLiteral("Harbor Reel"));
        QCOMPARE(restored.nodes().size(), graph.nodes().size());
        QCOMPARE(restored.connections().size(), graph.connections().size());
        QCOMPARE(restored.nodes().first().promptText,
                 QStringLiteral("dawn over the harbor"));
    }

    void loadReportsAbsentDocument()
    {
        QTemporaryDir dir;
        core::NodeGraph graph;
        WorkflowStore store(&graph);
        store.setDirectory(dir.path());
        QVERIFY(!store.load());
        QVERIFY(graph.nodes().isEmpty());
    }

    void burstEditsCoalesceIntoOneWrite()
    {
        QTemporaryDir dir;
        core::NodeGraph graph = sampleBoard();
        WorkflowStore store(&graph);
        store.setDirectory(dir.path());

        QSignalSpy states(&store, &WorkflowStore::stateChanged);
        store.scheduleSave();
        store.scheduleSave();
        store.scheduleSave();
        QCOMPARE(store.state(), WorkflowStore::State::Pending);
        QVERIFY(!QFile::exists(store.filePath()));

        QTRY_COMPARE_WITH_TIMEOUT(store.state(), WorkflowStore::State::Saved,
                                  3000);
        QVERIFY(QFile::exists(store.filePath()));

        // Exactly one Pending → Saved pair: the burst became one write.
        QCOMPARE(states.count(), 2);
    }

    void flushWritesAPendingSaveImmediately()
    {
        QTemporaryDir dir;
        core::NodeGraph graph = sampleBoard();
        WorkflowStore store(&graph);
        store.setDirectory(dir.path());

        store.scheduleSave();
        QCOMPARE(store.state(), WorkflowStore::State::Pending);
        QVERIFY(!QFile::exists(store.filePath()));

        // The quit path: the debounce window must not swallow the edit.
        store.flushPendingSave();
        QCOMPARE(store.state(), WorkflowStore::State::Saved);
        QVERIFY(QFile::exists(store.filePath()));

        // With nothing pending, a flush writes nothing.
        QFile::remove(store.filePath());
        store.flushPendingSave();
        QVERIFY(!QFile::exists(store.filePath()));
    }

    void nameFallsBackAndPersists()
    {
        QTemporaryDir dir;
        core::NodeGraph graph = sampleBoard();
        WorkflowStore store(&graph);
        store.setDirectory(dir.path());

        QSignalSpy names(&store, &WorkflowStore::nameChanged);
        store.setName(QStringLiteral("   "));
        QCOMPARE(store.name(), QStringLiteral("Untitled Workflow"));
        store.setName(QStringLiteral("Night Shoot"));
        QCOMPARE(names.count(), 1);
        QVERIFY(store.saveNow());

        core::NodeGraph restored;
        WorkflowStore reader(&restored);
        reader.setDirectory(dir.path());
        QVERIFY(reader.load());
        QCOMPARE(reader.name(), QStringLiteral("Night Shoot"));
    }

    void quickBindingPersistsAcrossSaveAndLoad()
    {
        QTemporaryDir dir;
        core::NodeGraph graph = sampleBoard();
        const QString boundUid = graph.nodes().last().uid;
        QVERIFY(!boundUid.isEmpty());

        WorkflowStore store(&graph);
        store.setDirectory(dir.path());
        store.setQuickNodeUid(boundUid);
        QVERIFY(store.saveNow());

        core::NodeGraph restored;
        WorkflowStore reader(&restored);
        reader.setDirectory(dir.path());
        QVERIFY(reader.load());
        QCOMPARE(reader.quickNodeUid(), boundUid);
        QVERIFY(restored.nodeByUid(boundUid));
        QCOMPARE(restored.nodeByUid(boundUid)->id, graph.nodes().last().id);
    }

    void legacyQuickTitleMigratesIntoTheBinding()
    {
        // A stored document from before the binding existed named its quick
        // node only by title; loading it adopts that node's identity once.
        QTemporaryDir dir;
        core::NodeGraph graph = sampleBoard();
        core::Node quick =
            core::catalogPrototype(QStringLiteral("Generate Image"));
        quick.title = QStringLiteral("Quick Generate");
        const int quickId = graph.addNode(quick);

        WorkflowStore store(&graph);
        store.setDirectory(dir.path());
        QVERIFY(store.saveNow());
        QVERIFY(store.quickNodeUid().isEmpty());

        core::NodeGraph restored;
        WorkflowStore reader(&restored);
        reader.setDirectory(dir.path());
        QVERIFY(reader.load());
        QCOMPARE(reader.quickNodeUid(), restored.nodeById(quickId)->uid);
    }

    void unwritableDirectoryReportsFailure()
    {
        QTemporaryDir dir;
        // A path under a plain file can never become a directory.
        const QString blocker = dir.path() + QStringLiteral("/blocker");
        {
            QFile file(blocker);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        core::NodeGraph graph = sampleBoard();
        WorkflowStore store(&graph);
        store.setDirectory(blocker + QStringLiteral("/nested"));
        QVERIFY(!store.saveNow());
        QCOMPARE(store.state(), WorkflowStore::State::Failed);
        QVERIFY(!store.failureReason().isEmpty());
    }
};

QTEST_GUILESS_MAIN(WorkflowStoreTest)
#include "tst_workflowstore.moc"
