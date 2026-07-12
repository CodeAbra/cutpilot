#pragma once

#include "cutpilot/core/command/Command.h"

#include <memory>
#include <vector>

namespace cutpilot::core {

class NodeGraph;

// A bounded undo/redo history over owned commands. push applies a command and records
// it; record stores a command that the caller already applied (the live-dragged move
// path); undo and redo walk a cursor over the history. Any new push or record after an
// undo truncates the redo tail. Depth is capped: past the cap the oldest entry is
// dropped and undo stops there, keeping memory bounded over a long session. No Qt-GUI
// dependency, so the whole stack is headless-testable.
class CommandStack {
public:
    static constexpr int kDefaultMaxDepth = 200;

    explicit CommandStack(int maxDepth = kDefaultMaxDepth);

    // Apply the command against the graph, then record it (discarding any redo tail).
    void push(std::unique_ptr<Command> command, NodeGraph &graph);

    // Record an already-applied command without re-applying it (discarding any redo
    // tail). Used for a live-dragged move whose net delta is already in the model.
    void record(std::unique_ptr<Command> command);

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < int(m_commands.size()); }

    void undo(NodeGraph &graph);
    void redo(NodeGraph &graph);

    void clear();

    int depth() const { return int(m_commands.size()); }

private:
    void truncateRedoTail();
    void enforceDepthCap();

    std::vector<std::unique_ptr<Command>> m_commands;
    // Commands [0, m_cursor) are applied; [m_cursor, size) are the redo tail.
    int m_cursor = 0;
    int m_maxDepth;
};

} // namespace cutpilot::core
