#pragma once

#include "cutpilot/core/command/Command.h"

#include <memory>
#include <vector>

namespace cutpilot::core {

// A sequence of commands applied as one undo step. apply runs them in order;
// revert unwinds them in reverse. Used where one gesture makes several model
// edits, such as re-routing a connector (disconnect old, connect new).
class CompositeCommand : public Command {
public:
    void add(std::unique_ptr<Command> command);

    bool isEmpty() const { return m_commands.empty(); }

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    std::vector<std::unique_ptr<Command>> m_commands;
};

} // namespace cutpilot::core
