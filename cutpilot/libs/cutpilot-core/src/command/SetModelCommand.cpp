#include "cutpilot/core/command/SetModelCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

SetModelCommand::SetModelCommand(int nodeId, const QString &modelId,
                                 const QString &modelLabel)
    : m_nodeId(nodeId)
    , m_modelId(modelId)
    , m_modelLabel(modelLabel)
{
}

void SetModelCommand::apply(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node)
        return;
    if (!m_hasPrevious) {
        m_previousId = node->modelId;
        m_previousLabel = node->modelLabel;
        m_hasPrevious = true;
    }
    node->modelId = m_modelId;
    node->modelLabel = m_modelLabel;
    node->bumpContent();
}

void SetModelCommand::revert(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node || !m_hasPrevious)
        return;
    node->modelId = m_previousId;
    node->modelLabel = m_previousLabel;
    node->bumpContent();
}

} // namespace cutpilot::core
