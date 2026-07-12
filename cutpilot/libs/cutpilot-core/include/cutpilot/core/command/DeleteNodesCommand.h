#pragma once

#include "cutpilot/core/Node.h"
#include "cutpilot/core/command/Command.h"

#include <QVector>
#include <utility>

namespace cutpilot::core {

// Removes a set of nodes. apply snapshots each target as its (list index, node value)
// pair before removal, so revert re-inserts each at its original index with its
// original id and z-position. Nodes are removed in descending index order and restored
// in ascending order so the indices stay valid throughout.
class DeleteNodesCommand : public Command {
public:
    explicit DeleteNodesCommand(const QVector<int> &ids);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    QVector<int> m_ids;
    QVector<std::pair<int, Node>> m_snapshots; // ascending by index
    bool m_captured = false;
};

} // namespace cutpilot::core
