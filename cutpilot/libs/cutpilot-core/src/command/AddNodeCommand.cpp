#include "cutpilot/core/command/AddNodeCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

AddNodeCommand::AddNodeCommand(const Node &node)
    : m_node(node)
{
}

void AddNodeCommand::apply(NodeGraph &graph)
{
    if (!m_hasId) {
        m_node.id = graph.addNode(m_node);
        m_hasId = true;
        m_index = graph.indexOfId(m_node.id);
    } else {
        // Redo: re-create the identical node with its original id and z-position.
        graph.insertNode(m_index, m_node);
    }
}

void AddNodeCommand::revert(NodeGraph &graph)
{
    // Leave m_index at the position captured on the first apply. Re-deriving it here
    // would read whatever z the node currently sits at, so an intervening raise-to-top
    // would make redo restore the node at the wrong z.
    graph.removeNode(m_node.id);
}

} // namespace cutpilot::core
