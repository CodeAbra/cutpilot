#include "cutpilot/core/command/SetOutputFormatCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

SetOutputFormatCommand::SetOutputFormatCommand(int nodeId, int width, int height)
    : m_nodeId(nodeId)
    , m_width(width)
    , m_height(height)
{
}

void SetOutputFormatCommand::apply(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node)
        return;
    if (!m_hasPrevious) {
        m_previousWidth = node->outputWidth;
        m_previousHeight = node->outputHeight;
        m_hasPrevious = true;
    }
    node->outputWidth = m_width;
    node->outputHeight = m_height;
    node->bumpContent();
}

void SetOutputFormatCommand::revert(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node || !m_hasPrevious)
        return;
    node->outputWidth = m_previousWidth;
    node->outputHeight = m_previousHeight;
    node->bumpContent();
}

} // namespace cutpilot::core
