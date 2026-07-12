#pragma once

#include <QRectF>

#include "cutpilot/core/Node.h"

namespace cutpilot::render {

// World-space layout of a node card's regions: the slim header (title and,
// on a generation node, the model chip), the content body, and a generation
// node's footer carrying the progress band, status line, and run control.
// The mesh builder draws these regions, the content rasterizer lays text
// into them, and the layer hit-tests them — one definition keeps all three
// in agreement.
struct NodeCardLayout {
    static constexpr qreal kHeaderHeight = 30.0;
    static constexpr qreal kFooterHeight = 34.0;
    static constexpr qreal kProgressHeight = 3.0;
    static constexpr qreal kRunButtonWidth = 34.0;
    static constexpr qreal kChipWidth = 132.0;

    static bool hasFooter(const core::Node &node)
    {
        return node.kind == core::NodeKind::Generate;
    }

    static QRectF headerRect(const core::Node &node)
    {
        QRectF rect = node.worldRect();
        rect.setHeight(qMin(kHeaderHeight, rect.height()));
        return rect;
    }

    static QRectF footerRect(const core::Node &node)
    {
        if (!hasFooter(node))
            return QRectF();
        const QRectF rect = node.worldRect();
        const qreal height = qMin(kFooterHeight, rect.height() - kHeaderHeight);
        if (height <= 0.0)
            return QRectF();
        return QRectF(rect.left(), rect.bottom() - height, rect.width(), height);
    }

    static QRectF bodyRect(const core::Node &node)
    {
        const QRectF rect = node.worldRect();
        const qreal top = headerRect(node).bottom();
        const qreal bottom = hasFooter(node) ? footerRect(node).top() : rect.bottom();
        return QRectF(rect.left(), top, rect.width(), qMax(0.0, bottom - top));
    }

    static QRectF runButtonRect(const core::Node &node)
    {
        const QRectF footer = footerRect(node);
        if (footer.isEmpty())
            return QRectF();
        const qreal height = footer.height() - kProgressHeight - 8.0;
        return QRectF(footer.right() - kRunButtonWidth - 6.0,
                      footer.top() + kProgressHeight + 4.0, kRunButtonWidth, height);
    }

    static QRectF statusRect(const core::Node &node)
    {
        const QRectF footer = footerRect(node);
        if (footer.isEmpty())
            return QRectF();
        return QRectF(footer.left() + 10.0, footer.top() + kProgressHeight,
                      footer.width() - kRunButtonWidth - 24.0,
                      footer.height() - kProgressHeight);
    }

    static QRectF modelChipRect(const core::Node &node)
    {
        if (node.kind != core::NodeKind::Generate)
            return QRectF();
        const QRectF header = headerRect(node);
        const qreal width = qMin(kChipWidth, header.width() * 0.5);
        return QRectF(header.right() - width - 8.0, header.top() + 5.0, width,
                      header.height() - 10.0);
    }

    static QRectF progressRect(const core::Node &node)
    {
        const QRectF footer = footerRect(node);
        if (footer.isEmpty())
            return QRectF();
        return QRectF(footer.left(), footer.top(), footer.width(), kProgressHeight);
    }
};

} // namespace cutpilot::render
