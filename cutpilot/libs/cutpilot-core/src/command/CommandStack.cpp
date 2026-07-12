#include "cutpilot/core/command/CommandStack.h"

#include <algorithm>

namespace cutpilot::core {

CommandStack::CommandStack(int maxDepth)
    : m_maxDepth(maxDepth > 0 ? maxDepth : kDefaultMaxDepth)
{
}

void CommandStack::truncateRedoTail()
{
    m_commands.erase(m_commands.begin() + m_cursor, m_commands.end());
}

void CommandStack::enforceDepthCap()
{
    while (int(m_commands.size()) > m_maxDepth) {
        m_commands.erase(m_commands.begin());
        --m_cursor; // the dropped entry can no longer be undone
    }
    m_cursor = std::max(m_cursor, 0);
}

void CommandStack::push(std::unique_ptr<Command> command, NodeGraph &graph)
{
    if (!command)
        return;
    truncateRedoTail();
    command->apply(graph);
    m_commands.push_back(std::move(command));
    ++m_cursor;
    enforceDepthCap();
}

void CommandStack::record(std::unique_ptr<Command> command)
{
    if (!command)
        return;
    truncateRedoTail();
    m_commands.push_back(std::move(command));
    ++m_cursor;
    enforceDepthCap();
}

void CommandStack::undo(NodeGraph &graph)
{
    if (!canUndo())
        return;
    --m_cursor;
    m_commands[m_cursor]->revert(graph);
}

void CommandStack::redo(NodeGraph &graph)
{
    if (!canRedo())
        return;
    m_commands[m_cursor]->apply(graph);
    ++m_cursor;
}

void CommandStack::clear()
{
    m_commands.clear();
    m_cursor = 0;
}

} // namespace cutpilot::core
