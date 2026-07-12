#pragma once

#include "cutpilot/core/Node.h"

#include <QString>

namespace cutpilot::core {

// Whether the kind is one of the local GPU compositing operations — the
// nodes evaluated by the compositor rather than the generation service.
inline bool isCompositeKind(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Blend:
    case NodeKind::Mask:
    case NodeKind::Key:
    case NodeKind::Transform:
        return true;
    default:
        return false;
    }
}

// Whether the node can feed the compositor or the preview with pixels: a
// local source (a still, or a video's current frame), a compositing op, or a
// generation result.
inline bool producesImage(NodeKind kind)
{
    return kind == NodeKind::Still || kind == NodeKind::Video
        || kind == NodeKind::Generate || isCompositeKind(kind);
}

// A ready-to-place node of the given kind with its title, size, typed ports,
// and default parameters — one definition shared by the palette, board
// seeds, and tests so port indices never drift between them. Kinds without a
// prototype here return a Blank node.
Node compositeNodePrototype(NodeKind kind);

// The number of blend modes and each mode's human-readable name, for the
// inspector's mode picker.
int blendModeCount();
QString blendModeLabel(BlendMode mode);

} // namespace cutpilot::core
