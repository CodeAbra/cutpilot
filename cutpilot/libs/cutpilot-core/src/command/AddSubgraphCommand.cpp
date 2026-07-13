#include "cutpilot/core/command/AddSubgraphCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

AddSubgraphCommand::AddSubgraphCommand(const QVector<Node> &prototypes,
                                       const QVector<Connection> &indexWires)
    : m_indexWires(indexWires)
{
    m_adds.reserve(prototypes.size());
    for (const Node &prototype : prototypes)
        m_adds.push_back(std::make_unique<AddNodeCommand>(prototype));
}

void AddSubgraphCommand::apply(NodeGraph &graph)
{
    for (auto &add : m_adds)
        add->apply(graph);

    if (m_connects.empty()) {
        // The node ids exist only after the first add, so the edges are built
        // here by remapping each wire's prototype indices. An out-of-range
        // index yields an edge to a missing node, which ConnectCommand refuses.
        for (const Connection &wire : m_indexWires) {
            Connection connection = wire;
            connection.fromNodeId =
                wire.fromNodeId >= 0 && wire.fromNodeId < int(m_adds.size())
                ? m_adds[wire.fromNodeId]->nodeId()
                : -1;
            connection.toNodeId =
                wire.toNodeId >= 0 && wire.toNodeId < int(m_adds.size())
                ? m_adds[wire.toNodeId]->nodeId()
                : -1;
            m_connects.push_back(std::make_unique<ConnectCommand>(connection));
        }
    }

    for (auto &connect : m_connects)
        connect->apply(graph);
}

void AddSubgraphCommand::revert(NodeGraph &graph)
{
    for (auto it = m_connects.rbegin(); it != m_connects.rend(); ++it)
        (*it)->revert(graph);
    for (auto it = m_adds.rbegin(); it != m_adds.rend(); ++it)
        (*it)->revert(graph);
}

QVector<int> AddSubgraphCommand::nodeIds() const
{
    QVector<int> ids;
    ids.reserve(int(m_adds.size()));
    for (const auto &add : m_adds)
        ids.push_back(add->nodeId());
    return ids;
}

} // namespace cutpilot::core
