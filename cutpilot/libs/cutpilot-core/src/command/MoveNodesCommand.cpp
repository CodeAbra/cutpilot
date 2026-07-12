#include "cutpilot/core/command/MoveNodesCommand.h"

#include "cutpilot/core/NodeGraph.h"

namespace cutpilot::core {

MoveNodesCommand::MoveNodesCommand(const QVector<int> &ids, const QPointF &delta)
    : m_ids(ids)
    , m_delta(delta)
{
}

void MoveNodesCommand::apply(NodeGraph &graph)
{
    graph.moveNodesBy(m_ids, m_delta);
}

void MoveNodesCommand::revert(NodeGraph &graph)
{
    graph.moveNodesBy(m_ids, -m_delta);
}

} // namespace cutpilot::core
