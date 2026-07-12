#pragma once

#include "cutpilot/core/command/Command.h"

#include <QPointF>
#include <QVector>

namespace cutpilot::core {

// Translates a set of nodes by a net world delta. apply adds the delta; revert
// subtracts it. Recorded (not pushed) after a live drag so the whole gesture collapses
// into a single undoable step: the layer moves the nodes live during the drag, then
// records this net-delta command on release. Because record does not re-apply, the
// nodes stay at origin+delta rather than origin+2*delta.
class MoveNodesCommand : public Command {
public:
    MoveNodesCommand(const QVector<int> &ids, const QPointF &delta);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    QVector<int> m_ids;
    QPointF m_delta;
};

} // namespace cutpilot::core
