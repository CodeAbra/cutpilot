#pragma once

#include "cutpilot/core/Connection.h"
#include "cutpilot/core/command/Command.h"

namespace cutpilot::core {

// Removes one connection. apply snapshots the (list index, value) pair before
// removal, so revert re-inserts the edge at its original position with its
// original id.
class DisconnectCommand : public Command {
public:
    explicit DisconnectCommand(int connectionId);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    int m_id;
    Connection m_snapshot;
    int m_index = -1;
    bool m_captured = false;
};

} // namespace cutpilot::core
