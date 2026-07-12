#include "cutpilot/core/command/EditPromptCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

EditPromptCommand::EditPromptCommand(int nodeId, const QString &text)
    : m_nodeId(nodeId)
    , m_text(text)
{
}

void EditPromptCommand::apply(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node)
        return;
    if (!m_hasPrevious) {
        m_previousText = node->promptText;
        m_hasPrevious = true;
    }
    node->promptText = m_text;
    node->bumpContent();
}

void EditPromptCommand::revert(NodeGraph &graph)
{
    Node *node = graph.nodeById(m_nodeId);
    if (!node || !m_hasPrevious)
        return;
    node->promptText = m_previousText;
    node->bumpContent();
}

} // namespace cutpilot::core
