#include "cutpilot/core/command/ConnectCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

ConnectCommand::ConnectCommand(const Connection &connection)
    : m_connection(connection)
{
}

void ConnectCommand::apply(NodeGraph &graph)
{
    if (!m_captured) {
        m_captured = true;
        // A stale endpoint refuses the whole command before the occupant is
        // touched, so a refused connect leaves nothing for revert to restore.
        m_refused = !graph.connectionEndpointsValid(m_connection);
        if (m_refused)
            return;
        const int existingId =
            graph.connectionAtInput(m_connection.toNodeId, m_connection.toPortIndex);
        if (existingId != -1) {
            m_replacedIndex = graph.connectionIndexOfId(existingId);
            m_replaced = *graph.connectionById(existingId);
            graph.removeConnection(existingId);
        }
        m_connection.id = graph.addConnection(m_connection);
        m_index = graph.connectionIndexOfId(m_connection.id);
        return;
    }
    if (m_refused)
        return;

    // Redo: remove the restored occupant again, then re-create the identical edge.
    if (m_replacedIndex != -1)
        graph.removeConnection(m_replaced.id);
    graph.insertConnection(m_index, m_connection);
}

void ConnectCommand::revert(NodeGraph &graph)
{
    if (m_refused)
        return;
    graph.removeConnection(m_connection.id);
    if (m_replacedIndex != -1)
        graph.insertConnection(m_replacedIndex, m_replaced);
}

} // namespace cutpilot::core
