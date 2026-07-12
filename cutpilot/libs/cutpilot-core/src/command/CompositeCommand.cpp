#include "cutpilot/core/command/CompositeCommand.h"

namespace cutpilot::core {

void CompositeCommand::add(std::unique_ptr<Command> command)
{
    m_commands.push_back(std::move(command));
}

void CompositeCommand::apply(NodeGraph &graph)
{
    for (auto &command : m_commands)
        command->apply(graph);
}

void CompositeCommand::revert(NodeGraph &graph)
{
    for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
        (*it)->revert(graph);
}

} // namespace cutpilot::core
