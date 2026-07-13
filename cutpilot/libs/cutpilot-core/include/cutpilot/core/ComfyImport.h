#pragma once

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/command/CommandStack.h"

#include <QJsonObject>
#include <QPointF>
#include <QVector>

namespace cutpilot::core {

// One row of the import's per-node grading: how the foreign node landed on
// the canvas — exact, substituted, passthrough, or unresolved.
struct ComfyImportRow {
    int comfyId = 0;
    QString comfyType;
    QString tier;
    QString mappedKind;
};

struct ComfyImportOutcome {
    bool ok = false;
    QString error;

    // The created canvas nodes, in the workflow's node order.
    QVector<int> nodeIds;
    int connectionCount = 0;

    // Workflow links whose types fit no port pairing on the mapped nodes.
    int droppedEdges = 0;

    QVector<ComfyImportRow> report;
};

// Land the convert service's ComfyUI import result on the canvas as one
// undoable step: every foreign node becomes a card — mapped kinds with
// their payloads, unknown ones preserved whole as foreign cards — placed at
// its workflow position offset by origin, and every surviving link becomes
// a wire between type-compatible ports.
ComfyImportOutcome applyComfyImport(NodeGraph &graph, CommandStack &commands,
                                    const QJsonObject &result,
                                    const QPointF &origin = QPointF(0, 0));

} // namespace cutpilot::core
