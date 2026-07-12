#include "cutpilot/core/command/SetCompositeParamsCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

SetCompositeParamsCommand::SetCompositeParamsCommand(int nodeId,
                                                     const CompositeParams &before,
                                                     const CompositeParams &after)
    : m_nodeId(nodeId)
    , m_before(before)
    , m_after(after)
{
}

void SetCompositeParamsCommand::write(NodeGraph &graph, const CompositeParams &params)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node)
        return;
    node->comp = params;
    node->bumpContent();
}

void SetCompositeParamsCommand::apply(NodeGraph &graph)
{
    write(graph, m_after);
}

void SetCompositeParamsCommand::revert(NodeGraph &graph)
{
    write(graph, m_before);
}

} // namespace cutpilot::core
