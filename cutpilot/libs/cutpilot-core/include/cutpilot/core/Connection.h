#pragma once

namespace cutpilot::core {

// A directed edge from one node's output port to another node's input port. Port
// indices index into the owning node's port list.
struct Connection {
    int id = 0;
    int fromNodeId = 0;
    int fromPortIndex = 0;
    int toNodeId = 0;
    int toPortIndex = 0;

    bool touchesNode(int nodeId) const
    {
        return fromNodeId == nodeId || toNodeId == nodeId;
    }
};

} // namespace cutpilot::core
