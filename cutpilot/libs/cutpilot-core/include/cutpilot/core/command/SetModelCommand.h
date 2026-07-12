#pragma once

#include "cutpilot/core/command/Command.h"

#include <QString>

namespace cutpilot::core {

// Picks the generation model a node runs with. The first apply snapshots the
// prior choice, so undo restores it; both directions bump the node's content
// revision so the header chip refreshes.
class SetModelCommand : public Command {
public:
    SetModelCommand(int nodeId, const QString &modelId, const QString &modelLabel);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    int m_nodeId;
    QString m_modelId;
    QString m_modelLabel;
    QString m_previousId;
    QString m_previousLabel;
    bool m_hasPrevious = false;
};

} // namespace cutpilot::core
