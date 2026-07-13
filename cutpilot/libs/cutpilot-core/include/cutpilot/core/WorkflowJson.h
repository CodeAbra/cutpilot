#pragma once

#include <QJsonObject>
#include <QString>

namespace cutpilot::core {

class NodeGraph;

// The workflow document: the whole node graph plus its display name as JSON.
// Node and connection ids are preserved exactly, so a reloaded board is
// byte-for-byte the same graph the autosave captured — including ids the undo
// history and caches key off. Transient run status is not part of the document;
// finished result paths are, so a reopened board shows its media again.
QJsonObject workflowToJson(const NodeGraph &graph, const QString &name);

// Populate an empty graph from a workflow document. Returns false — leaving the
// graph untouched — when the payload is not a workflow document or any node or
// connection fails validation.
bool workflowFromJson(const QJsonObject &json, NodeGraph &graph,
                      QString *name = nullptr);

} // namespace cutpilot::core
