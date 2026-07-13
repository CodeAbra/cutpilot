#pragma once

#include "cutpilot/core/Connection.h"
#include "cutpilot/core/Node.h"
#include "cutpilot/core/command/AddNodeCommand.h"
#include "cutpilot/core/command/ConnectCommand.h"

#include <QVector>

#include <memory>
#include <vector>

namespace cutpilot::core {

// Adds a whole wired subgraph — a dropped template, a duplicated selection — as
// one undo step. The wires reference the prototype list by index, because the
// real node ids exist only after the first apply; from then on every apply
// re-creates the identical nodes and edges with their original ids, so undo and
// redo restore the drop exactly.
class AddSubgraphCommand : public Command {
public:
    AddSubgraphCommand(const QVector<Node> &prototypes,
                       const QVector<Connection> &indexWires);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

    // The placed nodes' assigned ids, valid after the first apply.
    QVector<int> nodeIds() const;

private:
    std::vector<std::unique_ptr<AddNodeCommand>> m_adds;
    QVector<Connection> m_indexWires;
    std::vector<std::unique_ptr<ConnectCommand>> m_connects;
};

} // namespace cutpilot::core
