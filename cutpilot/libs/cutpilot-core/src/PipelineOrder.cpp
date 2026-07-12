#include "cutpilot/core/PipelineOrder.h"

namespace cutpilot::core {

namespace {

// Node-level wire maps: for each node, the nodes wired into it and out of it.
struct Adjacency {
    QHash<int, QVector<int>> incoming;
    QHash<int, QVector<int>> outgoing;
};

Adjacency buildAdjacency(const NodeGraph &graph)
{
    Adjacency adjacency;
    for (const Connection &edge : graph.connections()) {
        adjacency.incoming[edge.toNodeId].push_back(edge.fromNodeId);
        adjacency.outgoing[edge.fromNodeId].push_back(edge.toNodeId);
    }
    return adjacency;
}

QSet<int> closure(const NodeGraph &graph, int nodeId,
                  const QHash<int, QVector<int>> &neighbours)
{
    QSet<int> seen;
    if (!graph.nodeById(nodeId))
        return seen;
    QVector<int> frontier{nodeId};
    seen.insert(nodeId);
    while (!frontier.isEmpty()) {
        const int current = frontier.takeLast();
        for (int next : neighbours.value(current)) {
            if (seen.contains(next) || !graph.nodeById(next))
                continue;
            seen.insert(next);
            frontier.push_back(next);
        }
    }
    return seen;
}

} // namespace

QHash<int, QSet<int>> generationDependencies(const NodeGraph &graph)
{
    const Adjacency adjacency = buildAdjacency(graph);

    QHash<int, QSet<int>> dependencies;
    for (const Node &node : graph.nodes()) {
        if (!isEvaluatable(node))
            continue;
        QSet<int> deps;
        QSet<int> seen;
        QVector<int> frontier{node.id};
        while (!frontier.isEmpty()) {
            const int current = frontier.takeLast();
            for (int sourceId : adjacency.incoming.value(current)) {
                // A wire path that loops back to the node itself is a
                // dependency on its own output — a cycle the ordering below
                // must refuse.
                if (sourceId == node.id) {
                    deps.insert(node.id);
                    continue;
                }
                if (seen.contains(sourceId))
                    continue;
                seen.insert(sourceId);
                const Node *source = graph.nodeById(sourceId);
                if (!source)
                    continue;
                if (isEvaluatable(*source))
                    deps.insert(sourceId);
                else
                    frontier.push_back(sourceId);
            }
        }
        dependencies.insert(node.id, deps);
    }
    return dependencies;
}

EvaluationPlan evaluationOrder(const NodeGraph &graph, const QVector<int> &subset)
{
    const QHash<int, QSet<int>> allDependencies = generationDependencies(graph);
    const QSet<int> members(subset.cbegin(), subset.cend());

    // Restrict each member's dependencies to the subset; anything outside is
    // treated as already satisfied.
    QHash<int, QSet<int>> pending;
    QHash<int, QVector<int>> dependants;
    for (int id : subset) {
        QSet<int> deps;
        for (int dep : allDependencies.value(id)) {
            if (members.contains(dep) || dep == id)
                deps.insert(dep);
        }
        pending.insert(id, deps);
        for (int dep : deps)
            dependants[dep].push_back(id);
    }

    EvaluationPlan plan;
    plan.order.reserve(subset.size());

    // Kahn's algorithm, draining in the graph's node list order for a
    // deterministic result.
    QSet<int> emitted;
    bool progressed = true;
    while (progressed && plan.order.size() < subset.size()) {
        progressed = false;
        for (const Node &node : graph.nodes()) {
            if (!members.contains(node.id) || emitted.contains(node.id))
                continue;
            if (!pending.value(node.id).isEmpty())
                continue;
            emitted.insert(node.id);
            plan.order.push_back(node.id);
            for (int dependant : dependants.value(node.id))
                pending[dependant].remove(node.id);
            progressed = true;
        }
    }

    plan.hasCycle = plan.order.size() < subset.size();
    return plan;
}

QSet<int> upstreamClosure(const NodeGraph &graph, int nodeId)
{
    return closure(graph, nodeId, buildAdjacency(graph).incoming);
}

QSet<int> downstreamClosure(const NodeGraph &graph, int nodeId)
{
    return closure(graph, nodeId, buildAdjacency(graph).outgoing);
}

} // namespace cutpilot::core
