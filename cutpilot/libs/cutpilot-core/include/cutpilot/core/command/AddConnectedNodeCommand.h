#pragma once

#include "cutpilot/core/Node.h"
#include "cutpilot/core/command/AddNodeCommand.h"
#include "cutpilot/core/command/Command.h"
#include "cutpilot/core/command/ConnectCommand.h"

#include <memory>

namespace cutpilot::core {

// Adds a node and wires it to an existing port as one undo step: the gesture where
// dropping a connector on empty canvas places a new node already connected. The
// anchor names the existing port; the new node connects through its port at
// newNodePortIndex, as the target when the anchor is an output and as the source
// when the anchor is an input.
class AddConnectedNodeCommand : public Command {
public:
    AddConnectedNodeCommand(const Node &node, int anchorNodeId, int anchorPortIndex,
                            bool anchorIsOutput, int newNodePortIndex);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

    int nodeId() const { return m_add.nodeId(); }

private:
    AddNodeCommand m_add;
    std::unique_ptr<ConnectCommand> m_connect;
    int m_anchorNodeId;
    int m_anchorPortIndex;
    bool m_anchorIsOutput;
    int m_newNodePortIndex;
};

} // namespace cutpilot::core
