#include "cutpilot/core/command/SetMediaPathCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

SetMediaPathCommand::SetMediaPathCommand(int nodeId, const QString &mediaPath)
    : m_nodeId(nodeId)
    , m_mediaPath(mediaPath)
{
}

void SetMediaPathCommand::apply(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node)
        return;
    if (!m_hasPrevious) {
        m_previousPath = node->mediaPath;
        m_hasPrevious = true;
    }
    node->mediaPath = m_mediaPath;
    node->bumpContent();
}

void SetMediaPathCommand::revert(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node || !m_hasPrevious)
        return;
    node->mediaPath = m_previousPath;
    node->bumpContent();
}

} // namespace cutpilot::core
