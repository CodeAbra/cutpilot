#include <QtTest/QtTest>

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/WorkflowJson.h"
#include "cutpilot/core/command/AddSubgraphCommand.h"
#include "cutpilot/core/command/CommandStack.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace cutpilot;

namespace {

// A board touching every serialized surface: one node per kind with parameters
// off their defaults, plus wires.
core::NodeGraph fullBoard()
{
    core::NodeGraph graph;

    core::Node prompt = core::catalogPrototype(QStringLiteral("Prompt"));
    prompt.worldPos = QPointF(10.0, 20.0);
    prompt.promptText = QStringLiteral("dusk lighthouse");
    const int promptId = graph.addNode(prompt);

    core::Node generate = core::catalogPrototype(QStringLiteral("Generate Image"));
    generate.worldPos = QPointF(400.0, 60.0);
    generate.modelId = QStringLiteral("vendor/model-x");
    generate.modelLabel = QStringLiteral("Model X");
    generate.outputWidth = 1080;
    generate.outputHeight = 1440;
    generate.resultPath = QStringLiteral("/tmp/result.png");
    generate.resultDigest = QStringLiteral("abc123");
    generate.costUsd = 0.42;
    generate.resultWidth = 768;
    generate.resultHeight = 512;
    const int generateId = graph.addNode(generate);

    core::Node gate = core::catalogPrototype(QStringLiteral("Cost Gate"));
    gate.gateLimitUsd = 1.25;
    graph.addNode(gate);

    core::Node still = core::compositeNodePrototype(core::NodeKind::Still);
    still.mediaPath = QStringLiteral("/tmp/still.png");
    graph.addNode(still);

    core::Node key = core::compositeNodePrototype(core::NodeKind::Key);
    key.comp.keyColor = QColor(12, 200, 34, 255);
    key.comp.keyTolerance = 0.55;
    key.comp.lumaKey = true;
    graph.addNode(key);

    core::Node blend = core::compositeNodePrototype(core::NodeKind::Blend);
    blend.comp.blendMode = core::BlendMode::Screen;
    blend.comp.opacity = 0.4;
    graph.addNode(blend);

    core::Node frame = core::catalogPrototype(QStringLiteral("Frame"));
    frame.worldPos = QPointF(-100.0, -100.0);
    graph.addNode(frame);

    core::Node foreign;
    foreign.kind = core::NodeKind::Blank;
    foreign.title = QStringLiteral("KSampler");
    foreign.externalType = QStringLiteral("KSampler");
    foreign.externalData = QStringLiteral("{\"steps\":20}");
    foreign.ports = { { QStringLiteral("out"), core::PortType::Any, false, 0.5 } };
    graph.addNode(foreign);

    core::Connection wire;
    wire.fromNodeId = promptId;
    wire.fromPortIndex = 0;
    wire.toNodeId = generateId;
    wire.toPortIndex = 1;
    graph.addConnection(wire);
    return graph;
}

// The document as the pre-identity format wrote it: no uid on any node.
QJsonObject withoutUids(QJsonObject json)
{
    QJsonArray nodes = json[QLatin1String("nodes")].toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        QJsonObject node = nodes[i].toObject();
        node.remove(QLatin1String("uid"));
        nodes.replace(i, node);
    }
    json[QLatin1String("nodes")] = nodes;
    return json;
}

bool sameNode(const core::Node &a, const core::Node &b)
{
    if (a.id != b.id || a.uid != b.uid || a.title != b.title || a.kind != b.kind
        || a.worldPos != b.worldPos || a.worldSize != b.worldSize
        || a.promptText != b.promptText || a.modelId != b.modelId
        || a.modelLabel != b.modelLabel || a.outputWidth != b.outputWidth
        || a.outputHeight != b.outputHeight || a.gateLimitUsd != b.gateLimitUsd
        || a.mediaPath != b.mediaPath || a.externalType != b.externalType
        || a.externalData != b.externalData || !(a.comp == b.comp)
        || a.resultPath != b.resultPath || a.resultDigest != b.resultDigest
        || a.resultKind != b.resultKind
        || a.ports.size() != b.ports.size())
        return false;
    for (int i = 0; i < a.ports.size(); ++i) {
        const core::Port &pa = a.ports[i];
        const core::Port &pb = b.ports[i];
        if (pa.name != pb.name || pa.type != pb.type || pa.isInput != pb.isInput
            || pa.edgeFraction != pb.edgeFraction)
            return false;
    }
    return true;
}

} // namespace

class WorkflowJsonTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripPreservesEverything()
    {
        const core::NodeGraph board = fullBoard();
        const QJsonObject json =
            core::workflowToJson(board, QStringLiteral("Dusk Reel"));

        core::NodeGraph restored;
        QString name;
        QVERIFY(core::workflowFromJson(json, restored, &name));
        QCOMPARE(name, QStringLiteral("Dusk Reel"));
        QCOMPARE(restored.nodes().size(), board.nodes().size());
        QCOMPARE(restored.connections().size(), board.connections().size());
        for (int i = 0; i < board.nodes().size(); ++i)
            QVERIFY(sameNode(restored.nodes()[i], board.nodes()[i]));
        const core::Connection &wire = restored.connections().first();
        QCOMPARE(wire.id, board.connections().first().id);
        QCOMPARE(wire.fromNodeId, board.connections().first().fromNodeId);
        QCOMPARE(wire.toPortIndex, board.connections().first().toPortIndex);
    }

    void videoResultKindRoundTripsAndImageStaysEmpty()
    {
        core::NodeGraph graph;
        core::Node video = core::catalogPrototype(QStringLiteral("Generate Image"));
        video.resultPath = QStringLiteral("/tmp/result.mp4");
        video.resultKind = QStringLiteral("video");
        graph.addNode(video);
        core::Node image = core::catalogPrototype(QStringLiteral("Generate Image"));
        image.resultPath = QStringLiteral("/tmp/result.png");
        const int imageId = graph.addNode(image);

        const QJsonObject json = core::workflowToJson(graph, QString());

        // The image result writes no kind key; a document from before this
        // field then loads as an image.
        for (const QJsonValue &value :
             json[QLatin1String("nodes")].toArray()) {
            const QJsonObject node = value.toObject();
            if (node[QLatin1String("id")].toInt() == imageId)
                QVERIFY(!node.contains(QLatin1String("resultKind")));
        }

        core::NodeGraph restored;
        QVERIFY(core::workflowFromJson(json, restored, nullptr));
        QCOMPARE(restored.nodes()[0].resultKind, QStringLiteral("video"));
        QVERIFY(restored.nodes()[1].resultKind.isEmpty());
    }

    void restoredIdsDoNotCollideWithNewAdds()
    {
        const core::NodeGraph board = fullBoard();
        const QJsonObject json = core::workflowToJson(board, QString());

        core::NodeGraph restored;
        QVERIFY(core::workflowFromJson(json, restored, nullptr));
        const int newId = restored.addNode(core::Node());
        for (const core::Node &node : board.nodes())
            QVERIFY(node.id != newId);
    }

    void refusesForeignAndMalformedDocuments()
    {
        core::NodeGraph graph;
        QVERIFY(!core::workflowFromJson(QJsonObject(), graph, nullptr));

        // A wire to a missing node rejects the whole document, leaving the
        // graph untouched.
        QJsonObject json = core::workflowToJson(fullBoard(), QString());
        QJsonArray connections = json[QLatin1String("connections")].toArray();
        QJsonObject bad = connections.first().toObject();
        bad[QLatin1String("to")] = 9999;
        connections.push_back(bad);
        json[QLatin1String("connections")] = connections;
        QVERIFY(!core::workflowFromJson(json, graph, nullptr));
        QVERIFY(graph.nodes().isEmpty());

        // An unknown kind rejects too.
        QJsonObject unknownKind = core::workflowToJson(fullBoard(), QString());
        QJsonArray nodes = unknownKind[QLatin1String("nodes")].toArray();
        QJsonObject node = nodes.first().toObject();
        node[QLatin1String("kind")] = QStringLiteral("wormhole");
        nodes.replace(0, node);
        unknownKind[QLatin1String("nodes")] = nodes;
        QVERIFY(!core::workflowFromJson(unknownKind, graph, nullptr));
        QVERIFY(graph.nodes().isEmpty());

        // Non-finite geometry can never reach the loader through Qt's own
        // parser (an overflowing literal fails the parse outright) — and if
        // a value arrives non-finite anyway, the loader's finiteness guard
        // rejects the document. Both layers are pinned here.
        QString text = QString::fromUtf8(
            QJsonDocument(core::workflowToJson(fullBoard(), QString()))
                .toJson(QJsonDocument::Compact));
        text.replace(QStringLiteral("\"width\":260"),
                     QStringLiteral("\"width\":1e999"));
        QVERIFY(text.contains(QStringLiteral("1e999")));
        QVERIFY(QJsonDocument::fromJson(text.toUtf8()).isNull());

        // A populated graph refuses a load outright.
        core::NodeGraph occupied;
        occupied.addNode(core::Node());
        QVERIFY(!core::workflowFromJson(core::workflowToJson(fullBoard(), QString()),
                                        occupied, nullptr));
        QCOMPARE(occupied.nodes().size(), 1);
    }

    void outputSizesNormalizeToTheServiceContractOnLoad()
    {
        // A foreign document may carry any size; the loader holds both sides
        // inside the generation service's accepted range so a loaded node can
        // always run.
        QJsonObject json = core::workflowToJson(fullBoard(), QString());
        QJsonArray nodes = json[QLatin1String("nodes")].toArray();
        int generateIndex = -1;
        for (int i = 0; i < nodes.size(); ++i) {
            if (nodes[i].toObject()[QLatin1String("outputWidth")].toInt() > 0)
                generateIndex = i;
        }
        QVERIFY(generateIndex != -1);

        const auto loadWithSize = [&](QJsonValue width, QJsonValue height,
                                      core::NodeGraph &restored) {
            QJsonObject node = nodes[generateIndex].toObject();
            node[QLatin1String("outputWidth")] = width;
            node[QLatin1String("outputHeight")] = height;
            QJsonArray patched = nodes;
            patched.replace(generateIndex, node);
            QJsonObject document = json;
            document[QLatin1String("nodes")] = patched;
            QVERIFY(core::workflowFromJson(document, restored, nullptr));
        };

        const auto restoredSize = [&](const core::NodeGraph &restored) {
            for (const core::Node &node : restored.nodes()) {
                if (node.kind == core::NodeKind::Generate)
                    return QSize(node.outputWidth, node.outputHeight);
            }
            return QSize(-1, -1);
        };

        core::NodeGraph oversized;
        loadWithSize(9000, 9000, oversized);
        QCOMPARE(restoredSize(oversized), QSize(2048, 2048));

        core::NodeGraph undersized;
        loadWithSize(16, 16, undersized);
        QCOMPARE(restoredSize(undersized), QSize(64, 64));

        // A half-set pair is no format at all: the node returns to the
        // model-default marker instead of carrying one dangling side.
        core::NodeGraph halfSet;
        loadWithSize(1080, 0, halfSet);
        QCOMPARE(restoredSize(halfSet), QSize(0, 0));
    }

    void documentsWithoutUidsGainStableOnes()
    {
        // A document written before durable identities existed loads with a
        // fresh unique uid on every node, and those uids then persist: a
        // save/reload round trip returns the identical identities.
        const QJsonObject json =
            withoutUids(core::workflowToJson(fullBoard(), QString()));

        core::NodeGraph migrated;
        QVERIFY(core::workflowFromJson(json, migrated, nullptr));
        QStringList uids;
        for (const core::Node &node : migrated.nodes()) {
            QVERIFY(!node.uid.isEmpty());
            QVERIFY(!uids.contains(node.uid));
            uids.push_back(node.uid);
        }

        core::NodeGraph reloaded;
        QVERIFY(core::workflowFromJson(
            core::workflowToJson(migrated, QString()), reloaded, nullptr));
        for (int i = 0; i < migrated.nodes().size(); ++i)
            QCOMPARE(reloaded.nodes()[i].uid, migrated.nodes()[i].uid);
    }

    void duplicatedUidsKeepOnlyTheirFirstHolder()
    {
        QJsonObject json = core::workflowToJson(fullBoard(), QString());
        QJsonArray nodes = json[QLatin1String("nodes")].toArray();
        QJsonObject second = nodes[1].toObject();
        second[QLatin1String("uid")] = nodes[0].toObject()[QLatin1String("uid")];
        nodes.replace(1, second);
        json[QLatin1String("nodes")] = nodes;

        core::NodeGraph restored;
        QVERIFY(core::workflowFromJson(json, restored, nullptr));
        QCOMPARE(restored.nodes()[0].uid,
                 nodes[0].toObject()[QLatin1String("uid")].toString());
        QVERIFY(!restored.nodes()[1].uid.isEmpty());
        QVERIFY(restored.nodes()[1].uid != restored.nodes()[0].uid);
    }

    void quickBindingRoundTripsValidatesAndMigrates()
    {
        // The stored binding survives the round trip.
        const core::NodeGraph board = fullBoard();
        const QString boundUid = board.nodes()[1].uid;
        QVERIFY(!boundUid.isEmpty());
        core::NodeGraph restored;
        QString binding;
        QVERIFY(core::workflowFromJson(
            core::workflowToJson(board, QString(), boundUid), restored,
            nullptr, &binding));
        QCOMPARE(binding, boundUid);

        // A binding naming no node in the document is dropped, not adopted.
        core::NodeGraph dangling;
        QVERIFY(core::workflowFromJson(
            core::workflowToJson(board, QString(),
                                 QStringLiteral("no-such-node")),
            dangling, nullptr, &binding));
        QVERIFY(binding.isEmpty());

        // A binding naming a node of the wrong kind is dropped too: only a
        // generate node can back the quick surface.
        QCOMPARE(board.nodes()[0].kind, core::NodeKind::Prompt);
        core::NodeGraph wrongKind;
        QVERIFY(core::workflowFromJson(
            core::workflowToJson(board, QString(), board.nodes()[0].uid),
            wrongKind, nullptr, &binding));
        QVERIFY(binding.isEmpty());

        // A document from before identities existed — no uid stored on any
        // node — carries its quick node forward by the legacy title rule:
        // once, and the oldest id wins.
        core::NodeGraph legacyBoard;
        core::Node younger =
            core::catalogPrototype(QStringLiteral("Generate Image"));
        younger.title = QStringLiteral("Quick Generate");
        core::Node older = younger;
        const int firstId = legacyBoard.addNode(younger);
        legacyBoard.addNode(older);
        const QJsonObject legacyJson =
            withoutUids(core::workflowToJson(legacyBoard, QString()));
        QVERIFY(!legacyJson.contains(QLatin1String("quickNode")));
        core::NodeGraph adopted;
        QVERIFY(core::workflowFromJson(legacyJson, adopted, nullptr, &binding));
        QCOMPARE(binding, adopted.nodeById(firstId)->uid);

        // A current document stores identities; on one of those an absent
        // binding means exactly that. A look-alike node wearing the quick
        // title — a placed template carrying a saved copy — is never adopted.
        const QJsonObject lookAlike =
            core::workflowToJson(legacyBoard, QString());
        QVERIFY(!lookAlike.contains(QLatin1String("quickNode")));
        core::NodeGraph unclaimed;
        QVERIFY(core::workflowFromJson(lookAlike, unclaimed, nullptr,
                                       &binding));
        QVERIFY(binding.isEmpty());

        // No quick node at all: the binding stays empty.
        core::NodeGraph plain;
        QVERIFY(core::workflowFromJson(
            core::workflowToJson(fullBoard(), QString()), plain, nullptr,
            &binding));
        QVERIFY(binding.isEmpty());
    }

    void catalogTitlesAreUniqueAndPlaceable()
    {
        QStringList titles;
        for (const core::CatalogEntry &entry : core::nodeCatalog()) {
            QVERIFY(!entry.prototype.title.isEmpty());
            QVERIFY(!entry.category.isEmpty());
            QVERIFY(!titles.contains(entry.prototype.title));
            titles.push_back(entry.prototype.title);
        }
        QCOMPARE(core::catalogPrototype(QStringLiteral("Frame")).kind,
                 core::NodeKind::Frame);
        QCOMPARE(core::catalogPrototype(QStringLiteral("no such node")).kind,
                 core::NodeKind::Blank);
    }

    void subgraphPlacesAndUndoesAsOneStep()
    {
        core::NodeGraph graph;
        core::CommandStack stack;

        QVector<core::Node> protos = {
            core::catalogPrototype(QStringLiteral("Prompt")),
            core::catalogPrototype(QStringLiteral("Generate Image")),
        };
        protos[0].worldPos = QPointF(0.0, 0.0);
        protos[1].worldPos = QPointF(400.0, 0.0);

        core::Connection wire;
        wire.fromNodeId = 0; // prototype index
        wire.fromPortIndex = 0;
        wire.toNodeId = 1;
        wire.toPortIndex = 1;

        auto command =
            std::make_unique<core::AddSubgraphCommand>(protos, QVector{ wire });
        auto *raw = command.get();
        stack.push(std::move(command), graph);

        QCOMPARE(graph.nodes().size(), 2);
        QCOMPARE(graph.connections().size(), 1);
        const QVector<int> ids = raw->nodeIds();
        QCOMPARE(graph.connections().first().fromNodeId, ids[0]);
        QCOMPARE(graph.connections().first().toNodeId, ids[1]);

        // One undo step removes the whole drop; redo restores identical ids.
        stack.undo(graph);
        QVERIFY(graph.nodes().isEmpty());
        QVERIFY(graph.connections().isEmpty());
        stack.redo(graph);
        QCOMPARE(graph.nodes().size(), 2);
        QCOMPARE(graph.connections().size(), 1);
        QCOMPARE(graph.nodes()[0].id, ids[0]);
        QCOMPARE(graph.nodes()[1].id, ids[1]);
        QCOMPARE(graph.connections().first().fromNodeId, ids[0]);
    }

    void subgraphRefusesDanglingIndexWire()
    {
        core::NodeGraph graph;
        core::CommandStack stack;

        QVector<core::Node> protos = {
            core::catalogPrototype(QStringLiteral("Prompt")),
        };
        core::Connection wire;
        wire.fromNodeId = 0;
        wire.fromPortIndex = 0;
        wire.toNodeId = 7; // out of range
        wire.toPortIndex = 0;

        stack.push(std::make_unique<core::AddSubgraphCommand>(protos,
                                                              QVector{ wire }),
                   graph);
        QCOMPARE(graph.nodes().size(), 1);
        QVERIFY(graph.connections().isEmpty());
        stack.undo(graph);
        QVERIFY(graph.nodes().isEmpty());
    }
};

QTEST_APPLESS_MAIN(WorkflowJsonTest)
#include "tst_workflowjson.moc"
