#include "cutpilot/core/command/DeleteNodesCommand.h"

#include "cutpilot/core/NodeGraph.h"

#include <algorithm>

namespace cutpilot::core {

DeleteNodesCommand::DeleteNodesCommand(const QVector<int> &ids)
    : m_ids(ids)
{
}

void DeleteNodesCommand::apply(NodeGraph &graph)
{
    if (!m_captured) {
        m_snapshots.clear();
        for (int id : m_ids) {
            const int index = graph.indexOfId(id);
            const Node *node = graph.nodeById(id);
            if (index != NodeGraph::kNoIndex && node)
                m_snapshots.push_back({ index, *node });
        }
        std::sort(m_snapshots.begin(), m_snapshots.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        m_captured = true;
    }

    // Remove in descending index order so earlier indices stay valid.
    for (int i = m_snapshots.size() - 1; i >= 0; --i)
        graph.removeNode(m_snapshots[i].second.id);
}

void DeleteNodesCommand::revert(NodeGraph &graph)
{
    // Restore in ascending index order so each insertion lands at its captured slot.
    for (const auto &entry : m_snapshots)
        graph.insertNode(entry.first, entry.second);
}

} // namespace cutpilot::core
