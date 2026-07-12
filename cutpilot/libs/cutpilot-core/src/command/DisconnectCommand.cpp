#include "cutpilot/core/command/DisconnectCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

DisconnectCommand::DisconnectCommand(int connectionId)
    : m_id(connectionId)
{
}

void DisconnectCommand::apply(NodeGraph &graph)
{
    if (!m_captured) {
        m_index = graph.connectionIndexOfId(m_id);
        if (const Connection *c = graph.connectionById(m_id))
            m_snapshot = *c;
        m_captured = (m_index != NodeGraph::kNoIndex);
    }
    graph.removeConnection(m_id);
}

void DisconnectCommand::revert(NodeGraph &graph)
{
    if (m_captured)
        graph.insertConnection(m_index, m_snapshot);
}

} // namespace cutpilot::core
