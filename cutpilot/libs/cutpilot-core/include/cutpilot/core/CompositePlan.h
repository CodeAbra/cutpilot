#pragma once

#include "cutpilot/core/NodeGraph.h"

#include <QString>
#include <QVector>

namespace cutpilot::core {

// One wired input of a compositor pass. matte marks a Mask-typed upstream
// output: by convention a mask signal travels in the texture's alpha channel,
// so a consumer reads alpha where it would otherwise read color.
struct CompositeInput {
    int nodeId = -1; // -1: unwired, treated as fully transparent
    bool matte = false;

    bool operator==(const CompositeInput &other) const = default;
};

// One compositor pass: evaluate the node's operation over the outputs of its
// wired inputs. inputs holds one entry per input port, in port order.
struct CompositePass {
    int nodeId = 0;
    NodeKind kind = NodeKind::Blank;
    CompositeParams params;
    QVector<CompositeInput> inputs;
};

// The evaluation recipe for one target node: the sources whose pixels arrive
// from outside the compositor (stills and generation results) and the passes
// in dependency order, ending with the target itself. valid is false when
// the target produces no image or the walk found a cycle.
struct CompositePlan {
    bool valid = false;
    int targetNodeId = -1;
    QVector<int> sourceNodeIds;
    QVector<CompositePass> passes;
};

CompositePlan buildCompositePlan(const NodeGraph &graph, int targetNodeId);

// A stable signature of everything that shapes the node's composited pixels:
// the operation, its parameters, the wiring, and each source's identity —
// every field length-prefixed so no combination of values can collide with
// another. An unchanged signature means identical output, so a cached
// texture can stand in; any effective change anywhere upstream flips it.
QString compositeSignature(const NodeGraph &graph, int nodeId);

} // namespace cutpilot::core
