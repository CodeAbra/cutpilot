#pragma once

#include "cutpilot/core/command/Command.h"

namespace cutpilot::core {

// Sets a cost gate's spend limit. The first apply snapshots the prior limit,
// so undo restores exactly what the edit displaced. Both directions bump the
// node's content revision so its rendered body refreshes.
class SetGateLimitCommand : public Command {
public:
    SetGateLimitCommand(int nodeId, double limitUsd);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    int m_nodeId;
    double m_limitUsd;
    double m_previousLimitUsd = 0.0;
    bool m_hasPrevious = false;
};

} // namespace cutpilot::core
