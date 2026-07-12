#pragma once

#include "cutpilot/core/NodeGraph.h"

#include <QHash>
#include <QSet>
#include <QVector>

namespace cutpilot::core {

// Whether a node runs a generation job during a pipeline run. Prompt and
// gate nodes carry data or policy read synchronously; only generate nodes
// submit work.
inline bool isEvaluatable(const Node &node)
{
    return node.kind == NodeKind::Generate;
}

// The order a set of generate nodes must run in. When the set contains a
// dependency cycle it cannot be evaluated at all, and the run must be
// refused before anything is submitted.
struct EvaluationPlan {
    QVector<int> order;
    bool hasCycle = false;
};

// For every generate node: the upstream generate nodes it consumes, walking
// wires backwards through any non-generate nodes in between. A prompt or
// blank node passes reachability along; a generate node terminates the walk
// (its own inputs are its own concern).
QHash<int, QSet<int>> generationDependencies(const NodeGraph &graph);

// Dependency-first order of the given generate-node subset. Dependencies
// outside the subset are treated as already satisfied. Ties resolve in the
// graph's node list order, so the order is deterministic.
EvaluationPlan evaluationOrder(const NodeGraph &graph, const QVector<int> &subset);

// Every node the given node transitively consumes / feeds, over all wires,
// including the node itself. Safe on cyclic graphs.
QSet<int> upstreamClosure(const NodeGraph &graph, int nodeId);
QSet<int> downstreamClosure(const NodeGraph &graph, int nodeId);

} // namespace cutpilot::core
