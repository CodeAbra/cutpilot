#pragma once

#include "cutpilot/core/Node.h"
#include "cutpilot/core/command/Command.h"

namespace cutpilot::core {

// Adds one node. The first apply assigns and remembers the node's id; revert removes
// it; a later apply (redo) re-creates the identical node with the same id at the same
// z-position, so an added-then-undone node comes back unchanged.
class AddNodeCommand : public Command {
public:
    explicit AddNodeCommand(const Node &node);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

    int nodeId() const { return m_node.id; }

private:
    Node m_node;
    bool m_hasId = false;
    int m_index = 0;
};

} // namespace cutpilot::core
