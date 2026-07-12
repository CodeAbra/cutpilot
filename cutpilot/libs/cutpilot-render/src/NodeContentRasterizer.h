#pragma once

#include <QImage>

namespace cutpilot::core {
struct Node;
}

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::render {

// Paints a node's textual layer — title, model chip, prompt text, status
// line, cost and resolution — into a transparent image sized scale pixels
// per world unit. The image is uploaded as a texture stretched over the
// node's world rect, so text rides the card under pan and zoom without
// per-frame layout. Painting targets a QImage only, which Qt supports from
// any thread.
class NodeContentRasterizer {
public:
    // Pixels of texture per world unit; 2x keeps text crisp through the
    // detailed zoom range without oversized uploads.
    static constexpr qreal kScale = 2.0;

    QImage rasterize(const core::Node &node,
                     const theme::ThemeTable &theme) const;
};

} // namespace cutpilot::render
