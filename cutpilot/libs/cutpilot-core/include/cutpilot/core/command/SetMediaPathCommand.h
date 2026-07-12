#pragma once

#include "cutpilot/core/command/Command.h"

#include <QString>

namespace cutpilot::core {

// Points a still node at a different source file. The first apply snapshots
// the prior path, so undo restores exactly what the edit displaced. Both
// directions bump the node's content revision so its body reloads.
class SetMediaPathCommand : public Command {
public:
    SetMediaPathCommand(int nodeId, const QString &mediaPath);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    int m_nodeId;
    QString m_mediaPath;
    QString m_previousPath;
    bool m_hasPrevious = false;
};

} // namespace cutpilot::core
