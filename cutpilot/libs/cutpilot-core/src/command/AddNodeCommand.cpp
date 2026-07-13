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
        // The graph may mint the uid on add; capture it so redo restores the
        // node under the same durable identity.
        m_node.uid = graph.nodeById(m_node.id)->uid;
        m_hasId = true;
        m_index = graph.indexOfId(m_node.id);
    } else {
        // Redo: re-create the identical node with its original id and z-position.
        graph.insertNode(m_index, m_node);
    }
}

void AddNodeCommand::revert(NodeGraph &graph)
{
    // Re-capture the node's current value so redo restores exactly what this
    // undo removes — results and parameters written after the add (a finished
    // generation, a picked file) live on the node, not in later commands.
    // m_index stays at the position captured on the first apply: re-deriving
    // it here would read whatever z the node currently sits at, so an
    // intervening raise-to-top would make redo restore the wrong z. Selection
    // is view state written outside the stack; it stays as first captured so
    // a redo never replays whatever the selection happened to be at undo
    // time.
    if (const Node *current = graph.nodeById(m_node.id)) {
        const bool selected = m_node.selected;
        m_node = *current;
        m_node.selected = selected;
    }
    graph.removeNode(m_node.id);
}

} // namespace cutpilot::core
