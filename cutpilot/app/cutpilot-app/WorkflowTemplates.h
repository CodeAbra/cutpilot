#pragma once

#include "cutpilot/core/Connection.h"
#include "cutpilot/core/Node.h"

#include <QJsonObject>
#include <QVector>

namespace cutpilot::core {
class NodeGraph;
}

namespace cutpilot::app {

// A reusable building block: prototypes normalized to their joint origin and
// the wires among them re-expressed against prototype indices.
struct WorkflowTemplate {
    QVector<core::Node> prototypes;
    QVector<core::Connection> indexWires;
};

// Capture the graph's current selection as a template. Prototypes are
// pristine: ids, selection, run state, and any produced result data are
// stripped, so a placed template never claims a result nobody ran and never
// carries a past run's local file paths. Media source paths stay — they are
// the whole point of a still or video node.
WorkflowTemplate captureSelectionTemplate(const core::NodeGraph &graph);

// Template documents reuse the workflow format.
QJsonObject templateToJson(const WorkflowTemplate &content, const QString &name);
bool templateFromJson(const QJsonObject &json, WorkflowTemplate &content,
                      QString *name = nullptr);

} // namespace cutpilot::app
