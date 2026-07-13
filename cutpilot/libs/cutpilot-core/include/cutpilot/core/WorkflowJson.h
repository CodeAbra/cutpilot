#pragma once

#include <QJsonObject>
#include <QString>

namespace cutpilot::core {

class NodeGraph;

// The workflow document: the whole node graph plus its display name as JSON.
// Node and connection ids — and each node's durable uid — are preserved
// exactly, so a reloaded board is byte-for-byte the same graph the autosave
// captured, including the identities the undo history, caches, and bindings
// key off. quickNodeUid records which node the quick surface owns, so
// adoption survives renames and duplicates. Transient run status is not part
// of the document; finished result paths are, so a reopened board shows its
// media again.
QJsonObject workflowToJson(const NodeGraph &graph, const QString &name,
                           const QString &quickNodeUid = QString());

// Populate an empty graph from a workflow document. Returns false — leaving the
// graph untouched — when the payload is not a workflow document or any node or
// connection fails validation. Nodes missing a uid (documents written before
// uids existed, or duplicated by hand) leave with a freshly assigned one that
// then persists. quickNodeUid, when requested, is the validated stored
// binding; a document without one yields the oldest generate node still
// titled by the quick surface's original name, carrying the legacy binding
// forward once.
bool workflowFromJson(const QJsonObject &json, NodeGraph &graph,
                      QString *name = nullptr,
                      QString *quickNodeUid = nullptr);

} // namespace cutpilot::core
