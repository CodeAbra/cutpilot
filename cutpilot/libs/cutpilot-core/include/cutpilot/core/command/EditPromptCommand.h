#pragma once

#include "cutpilot/core/command/Command.h"

#include <QString>

namespace cutpilot::core {

// Replaces a node's prompt text. The first apply snapshots the prior text, so
// undo restores exactly what the edit displaced. Both directions bump the
// node's content revision so its rendered body refreshes.
class EditPromptCommand : public Command {
public:
    EditPromptCommand(int nodeId, const QString &text);

    void apply(NodeGraph &graph) override;
    void revert(NodeGraph &graph) override;

private:
    int m_nodeId;
    QString m_text;
    QString m_previousText;
    bool m_hasPrevious = false;
};

} // namespace cutpilot::core
