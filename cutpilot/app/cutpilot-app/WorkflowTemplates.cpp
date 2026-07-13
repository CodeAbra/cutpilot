#include "WorkflowTemplates.h"

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/WorkflowJson.h"

#include <QHash>

namespace cutpilot::app {

WorkflowTemplate captureSelectionTemplate(const core::NodeGraph &graph)
{
    WorkflowTemplate content;
    QHash<int, int> indexById;
    QRectF bounds;
    for (const core::Node &node : graph.nodes()) {
        if (!node.selected)
            continue;
        indexById.insert(node.id, content.prototypes.size());
        core::Node prototype = node;
        prototype.id = 0;
        // A template is a prototype, not an identity: every placement mints
        // its own uid, so a placed copy can never impersonate the original.
        prototype.uid.clear();
        prototype.selected = false;
        prototype.runState = core::RunState::Idle;
        prototype.runProgress = 0.0;
        prototype.statusMessage.clear();
        // A template is a starting point, not a record of one run: no result
        // artifacts, costs, or their local paths travel with it.
        prototype.resultPath.clear();
        prototype.resultDigest.clear();
        prototype.costUsd = -1.0;
        prototype.estimatedCostUsd = -1.0;
        prototype.gateSpentUsd = -1.0;
        prototype.resultWidth = 0;
        prototype.resultHeight = 0;
        content.prototypes.push_back(prototype);
        bounds = bounds.isNull() ? node.worldRect()
                                 : bounds.united(node.worldRect());
    }
    for (core::Node &prototype : content.prototypes)
        prototype.worldPos -= bounds.topLeft();

    for (const core::Connection &connection : graph.connections()) {
        if (!indexById.contains(connection.fromNodeId)
            || !indexById.contains(connection.toNodeId))
            continue;
        core::Connection wire;
        wire.fromNodeId = indexById.value(connection.fromNodeId);
        wire.fromPortIndex = connection.fromPortIndex;
        wire.toNodeId = indexById.value(connection.toNodeId);
        wire.toPortIndex = connection.toPortIndex;
        content.indexWires.push_back(wire);
    }
    return content;
}

QJsonObject templateToJson(const WorkflowTemplate &content, const QString &name)
{
    core::NodeGraph scratch;
    QVector<int> ids;
    for (const core::Node &prototype : content.prototypes)
        ids.push_back(scratch.addNode(prototype));
    for (const core::Connection &wire : content.indexWires) {
        core::Connection connection = wire;
        connection.fromNodeId = ids.value(wire.fromNodeId, -1);
        connection.toNodeId = ids.value(wire.toNodeId, -1);
        scratch.addConnection(connection);
    }
    return core::workflowToJson(scratch, name);
}

bool templateFromJson(const QJsonObject &json, WorkflowTemplate &content,
                      QString *name)
{
    core::NodeGraph scratch;
    if (!core::workflowFromJson(json, scratch, name))
        return false;
    QHash<int, int> indexById;
    for (const core::Node &node : scratch.nodes()) {
        indexById.insert(node.id, content.prototypes.size());
        core::Node prototype = node;
        prototype.id = 0;
        prototype.uid.clear();
        content.prototypes.push_back(prototype);
    }
    for (const core::Connection &connection : scratch.connections()) {
        core::Connection wire;
        wire.fromNodeId = indexById.value(connection.fromNodeId);
        wire.fromPortIndex = connection.fromPortIndex;
        wire.toNodeId = indexById.value(connection.toNodeId);
        wire.toPortIndex = connection.toPortIndex;
        content.indexWires.push_back(wire);
    }
    return true;
}

} // namespace cutpilot::app
