#include "cutpilot/core/command/SetGateLimitCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

SetGateLimitCommand::SetGateLimitCommand(int nodeId, double limitUsd)
    : m_nodeId(nodeId)
    , m_limitUsd(limitUsd)
{
}

void SetGateLimitCommand::apply(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node)
        return;
    if (!m_hasPrevious) {
        m_previousLimitUsd = node->gateLimitUsd;
        m_hasPrevious = true;
    }
    node->gateLimitUsd = m_limitUsd;
    node->bumpContent();
}

void SetGateLimitCommand::revert(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node || !m_hasPrevious)
        return;
    node->gateLimitUsd = m_previousLimitUsd;
    node->bumpContent();
}

} // namespace cutpilot::core
