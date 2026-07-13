#include "cutpilot/core/ComfyImport.h"

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/PortRules.h"
#include "cutpilot/core/command/Command.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPair>
#include <QSet>

#include <memory>

namespace cutpilot::core {

namespace {

// Adds a set of nodes and the wires between them as one undo step. Ids and
// z-positions are assigned on the first apply and reproduced exactly on
// redo, mirroring how a single added node comes back unchanged.
class ImportWorkflowCommand : public Command {
public:
    struct Edge {
        int fromIndex = -1;
        int fromPort = -1;
        int toIndex = -1;
        int toPort = -1;
    };

    ImportWorkflowCommand(QVector<Node> nodes, QVector<Edge> edges)
        : m_nodes(std::move(nodes))
        , m_edges(std::move(edges))
    {
    }

    void apply(NodeGraph &graph) override
    {
        if (!m_applied) {
            for (Node &node : m_nodes) {
                node.id = graph.addNode(node);
                // Capture the minted uid so redo restores the same identity.
                node.uid = graph.nodeById(node.id)->uid;
                m_nodeIndices.append(graph.indexOfId(node.id));
            }
            for (const Edge &edge : m_edges) {
                Connection connection;
                connection.fromNodeId = m_nodes.at(edge.fromIndex).id;
                connection.fromPortIndex = edge.fromPort;
                connection.toNodeId = m_nodes.at(edge.toIndex).id;
                connection.toPortIndex = edge.toPort;
                const int id = graph.addConnection(connection);
                if (id == -1)
                    continue;
                connection.id = id;
                m_connections.append(connection);
                m_connectionIndices.append(graph.connectionIndexOfId(id));
            }
            m_applied = true;
            return;
        }
        for (int i = 0; i < m_nodes.size(); ++i)
            graph.insertNode(m_nodeIndices.at(i), m_nodes.at(i));
        for (int i = 0; i < m_connections.size(); ++i)
            graph.insertConnection(m_connectionIndices.at(i),
                                   m_connections.at(i));
    }

    void revert(NodeGraph &graph) override
    {
        for (int i = m_connections.size() - 1; i >= 0; --i)
            graph.removeConnection(m_connections.at(i).id);
        for (int i = m_nodes.size() - 1; i >= 0; --i)
            graph.removeNode(m_nodes.at(i).id);
    }

    QVector<int> nodeIds() const
    {
        QVector<int> ids;
        ids.reserve(m_nodes.size());
        for (const Node &node : m_nodes)
            ids.append(node.id);
        return ids;
    }

    int connectionCount() const { return m_connections.size(); }

private:
    QVector<Node> m_nodes;
    QVector<Edge> m_edges;
    QVector<Connection> m_connections;
    QVector<int> m_nodeIndices;
    QVector<int> m_connectionIndices;
    bool m_applied = false;
};

Node promptPrototype()
{
    Node node;
    node.kind = NodeKind::Prompt;
    node.title = QStringLiteral("Prompt");
    node.worldSize = QSizeF(260.0, 170.0);
    node.ports = {
        { QStringLiteral("text"), PortType::Text, false, 0.5 },
    };
    return node;
}

Node generatePrototype()
{
    Node node;
    node.kind = NodeKind::Generate;
    node.title = QStringLiteral("Generate Image");
    node.worldSize = QSizeF(280.0, 200.0);
    node.ports = {
        { QStringLiteral("image"), PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), PortType::Text, true, 0.55 },
        { QStringLiteral("run"), PortType::Control, true, 0.8 },
        { QStringLiteral("result"), PortType::Image, false, 0.5 },
    };
    return node;
}

Node nodeForSpec(const QJsonObject &spec, const QPointF &origin)
{
    const QString kind = spec.value(QStringLiteral("kind")).toString();
    Node node;
    if (kind == QStringLiteral("prompt")) {
        node = promptPrototype();
        node.promptText = spec.value(QStringLiteral("prompt")).toString();
    } else if (kind == QStringLiteral("generate")) {
        node = generatePrototype();
    } else if (kind == QStringLiteral("still")) {
        node = compositeNodePrototype(NodeKind::Still);
    } else if (kind == QStringLiteral("video")) {
        node = compositeNodePrototype(NodeKind::Video);
    } else if (kind == QStringLiteral("blend")) {
        node = compositeNodePrototype(NodeKind::Blend);
    } else if (kind == QStringLiteral("mask")) {
        node = compositeNodePrototype(NodeKind::Mask);
    } else if (kind == QStringLiteral("key")) {
        node = compositeNodePrototype(NodeKind::Key);
    } else if (kind == QStringLiteral("transform")) {
        node = compositeNodePrototype(NodeKind::Transform);
    } else {
        node = compositeNodePrototype(NodeKind::Blank);
    }

    const QString title = spec.value(QStringLiteral("title")).toString();
    if (!title.isEmpty())
        node.title = title;

    // A still's media entry names a file inside the foreign system's own
    // library — meaningless as a local path. Surface the name; the user
    // picks the real file.
    const QString media = spec.value(QStringLiteral("media")).toString();
    if (!media.isEmpty() && node.kind == NodeKind::Still)
        node.title = media;

    const QString comfyType =
        spec.value(QStringLiteral("comfy_type")).toString();
    const QJsonValue opaque = spec.value(QStringLiteral("opaque"));
    if (opaque.isObject() && !opaque.toObject().isEmpty()) {
        node.externalType = comfyType;
        node.externalData = QString::fromUtf8(
            QJsonDocument(opaque.toObject()).toJson(QJsonDocument::Compact));
        if (title.isEmpty() && !comfyType.isEmpty())
            node.title = comfyType;
    }

    const QJsonArray pos = spec.value(QStringLiteral("pos")).toArray();
    node.worldPos = origin
        + QPointF(pos.size() > 0 ? pos.at(0).toDouble() : 0.0,
                  pos.size() > 1 ? pos.at(1).toDouble() : 0.0);
    return node;
}

// The first type-compatible output→input pairing between the two nodes
// whose input port no earlier link already claimed — an input holds at most
// one edge, so a taken port never absorbs a second link. Direct matches win
// over converted ones. {-1, -1} when nothing free fits.
QPair<int, int> pickPorts(const Node &from, const Node &to, int toIndex,
                          const QSet<QPair<int, int>> &takenInputs)
{
    QPair<int, int> converted{ -1, -1 };
    for (int f = 0; f < from.ports.size(); ++f) {
        if (from.ports.at(f).isInput)
            continue;
        for (int t = 0; t < to.ports.size(); ++t) {
            if (!to.ports.at(t).isInput
                || takenInputs.contains({ toIndex, t }))
                continue;
            const PortMatch match =
                portMatch(from.ports.at(f).type, to.ports.at(t).type);
            if (match == PortMatch::Direct)
                return { f, t };
            if (match == PortMatch::Converted && converted.first == -1)
                converted = { f, t };
        }
    }
    return converted;
}

} // namespace

ComfyImportOutcome applyComfyImport(NodeGraph &graph, CommandStack &commands,
                                    const QJsonObject &result,
                                    const QPointF &origin)
{
    ComfyImportOutcome outcome;

    const QJsonArray specs = result.value(QStringLiteral("nodes")).toArray();
    if (specs.isEmpty()) {
        outcome.error = QStringLiteral("the workflow mapped to no nodes");
        return outcome;
    }

    QVector<Node> nodes;
    QHash<int, int> indexByComfyId;
    for (const QJsonValue &value : specs) {
        const QJsonObject spec = value.toObject();
        indexByComfyId.insert(spec.value(QStringLiteral("comfy_id")).toInt(),
                              nodes.size());
        nodes.append(nodeForSpec(spec, origin));
    }

    QVector<ImportWorkflowCommand::Edge> edges;
    QSet<QPair<int, int>> takenInputs;
    const QJsonArray links =
        result.value(QStringLiteral("connections")).toArray();
    for (const QJsonValue &value : links) {
        const QJsonObject link = value.toObject();
        const int fromIndex = indexByComfyId.value(
            link.value(QStringLiteral("from")).toInt(), -1);
        const int toIndex =
            indexByComfyId.value(link.value(QStringLiteral("to")).toInt(), -1);
        if (fromIndex < 0 || toIndex < 0) {
            ++outcome.droppedEdges;
            continue;
        }
        const QPair<int, int> ports = pickPorts(
            nodes.at(fromIndex), nodes.at(toIndex), toIndex, takenInputs);
        if (ports.first < 0) {
            ++outcome.droppedEdges;
            continue;
        }
        takenInputs.insert({ toIndex, ports.second });
        edges.append({ fromIndex, ports.first, toIndex, ports.second });
    }

    auto command = std::make_unique<ImportWorkflowCommand>(nodes, edges);
    ImportWorkflowCommand *raw = command.get();
    commands.push(std::move(command), graph);

    outcome.ok = true;
    outcome.nodeIds = raw->nodeIds();
    outcome.connectionCount = raw->connectionCount();

    const QJsonArray report = result.value(QStringLiteral("report")).toArray();
    for (const QJsonValue &value : report) {
        const QJsonObject row = value.toObject();
        outcome.report.append(
            { row.value(QStringLiteral("id")).toInt(),
              row.value(QStringLiteral("type")).toString(),
              row.value(QStringLiteral("tier")).toString(),
              row.value(QStringLiteral("mapped")).toString() });
    }
    return outcome;
}

} // namespace cutpilot::core
