#pragma once

namespace cutpilot::core {

class NodeGraph;

// One reversible edit to the node graph. apply performs the edit; revert undoes it.
// Commands own whatever snapshot they need to reverse themselves, so they carry no
// Qt-GUI dependency and stay headless-testable.
class Command {
public:
    virtual ~Command() = default;

    virtual void apply(NodeGraph &graph) = 0;
    virtual void revert(NodeGraph &graph) = 0;
};

} // namespace cutpilot::core
