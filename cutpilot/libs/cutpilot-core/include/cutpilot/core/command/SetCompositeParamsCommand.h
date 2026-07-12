#pragma once

#include "cutpilot/core/Node.h"
#include "cutpilot/core/command/Command.h"

namespace cutpilot::core {

// Replaces a compositing node's parameters. Carries both sides explicitly so
// a live scrub — which already wrote the new values directly — can record the
// whole gesture as one step whose undo restores the pre-scrub state. Both
// directions bump the node's content revision so its body and any cached
// composite refresh.
class SetCompositeParamsCommand : public Command {
public:
    SetCompositeParamsCommand(int nodeId, const CompositeParams &before,
                              const CompositeParams &after);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    void write(NodeGraph &graph, const CompositeParams &params);

    int m_nodeId;
    CompositeParams m_before;
    CompositeParams m_after;
};

} // namespace cutpilot::core
