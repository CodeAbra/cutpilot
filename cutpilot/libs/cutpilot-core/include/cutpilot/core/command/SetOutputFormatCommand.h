#pragma once

#include "cutpilot/core/command/Command.h"

namespace cutpilot::core {

// Sets a generation node's requested output size in pixels; zero means the
// model's default. The first apply snapshots the prior size, so undo restores
// it; both directions bump the node's content revision so the format readout
// refreshes.
class SetOutputFormatCommand : public Command {
public:
    SetOutputFormatCommand(int nodeId, int width, int height);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    int m_nodeId;
    int m_width;
    int m_height;
    int m_previousWidth = 0;
    int m_previousHeight = 0;
    bool m_hasPrevious = false;
};

} // namespace cutpilot::core
