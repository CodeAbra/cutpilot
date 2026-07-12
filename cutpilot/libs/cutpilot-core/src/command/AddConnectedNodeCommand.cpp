#include "cutpilot/core/command/AddConnectedNodeCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

AddConnectedNodeCommand::AddConnectedNodeCommand(const Node &node, int anchorNodeId,
                                                 int anchorPortIndex, bool anchorIsOutput,
                                                 int newNodePortIndex)
    : m_add(node)
    , m_anchorNodeId(anchorNodeId)
    , m_anchorPortIndex(anchorPortIndex)
    , m_anchorIsOutput(anchorIsOutput)
    , m_newNodePortIndex(newNodePortIndex)
{
}

void AddConnectedNodeCommand::apply(NodeGraph &graph)
{
    m_add.apply(graph);

    if (!m_connect) {
        // The new node's id exists only after the first add, so the edge is built here.
        Connection connection;
        if (m_anchorIsOutput) {
            connection.fromNodeId = m_anchorNodeId;
            connection.fromPortIndex = m_anchorPortIndex;
            connection.toNodeId = m_add.nodeId();
            connection.toPortIndex = m_newNodePortIndex;
        } else {
            connection.fromNodeId = m_add.nodeId();
            connection.fromPortIndex = m_newNodePortIndex;
            connection.toNodeId = m_anchorNodeId;
            connection.toPortIndex = m_anchorPortIndex;
        }
        m_connect = std::make_unique<ConnectCommand>(connection);
    }

    m_connect->apply(graph);
}

void AddConnectedNodeCommand::revert(NodeGraph &graph)
{
    m_connect->revert(graph);
    m_add.revert(graph);
}

} // namespace cutpilot::core
