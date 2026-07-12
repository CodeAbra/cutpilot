#include "NodeContentRasterizer.h"
#include "NodeCardLayout.h"

#include "cutpilot/core/Node.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>

namespace cutpilot::render {

namespace {

QFont uiFont(qreal worldPixelSize, QFont::Weight weight = QFont::Normal)
{
    QFont font;
    font.setPixelSize(qRound(worldPixelSize));
    font.setWeight(weight);
    return font;
}

QFont numericFont(qreal worldPixelSize)
{
    QFont font;
    font.setFamilies({ QStringLiteral("SF Mono"), QStringLiteral("Menlo"),
                       QStringLiteral("Cascadia Code") });
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qRound(worldPixelSize));
    return font;
}

// The status line's text and color for a generation node.
struct StatusLine {
    QString text;
    QColor color;
};

StatusLine statusLine(const core::Node &node, const theme::ThemeTable &theme)
{
    switch (node.runState) {
    case core::RunState::Queued:
        return { node.statusMessage.isEmpty() ? QStringLiteral("Queued")
                                              : node.statusMessage,
                 theme.statusRunning() };
    case core::RunState::Running:
        return { QStringLiteral("%1 · %2%")
                     .arg(node.statusMessage.isEmpty()
                              ? QStringLiteral("Generating")
                              : node.statusMessage)
                     .arg(qRound(node.runProgress * 100.0)),
                 theme.statusRunning() };
    case core::RunState::Done: {
        QString line;
        if (node.costUsd >= 0.0)
            line = QStringLiteral("$%1").arg(node.costUsd, 0, 'f', 3);
        if (node.resultWidth > 0 && node.resultHeight > 0) {
            if (!line.isEmpty())
                line += QStringLiteral(" · ");
            line += QStringLiteral("%1×%2").arg(node.resultWidth).arg(node.resultHeight);
        }
        if (!node.statusMessage.isEmpty())
            line = node.statusMessage + QStringLiteral(" · ") + line;
        return { line, theme.statusDone() };
    }
    case core::RunState::Error:
        return { node.statusMessage.isEmpty() ? QStringLiteral("Generation failed")
                                              : node.statusMessage,
                 theme.statusError() };
    case core::RunState::NeedsKey:
        return { node.statusMessage.isEmpty() ? QStringLiteral("Add a key")
                                              : node.statusMessage,
                 theme.statusWarning() };
    case core::RunState::Idle:
        break;
    }
    return { node.statusMessage.isEmpty() ? QStringLiteral("Ready")
                                          : node.statusMessage,
             theme.textSecondary() };
}

} // namespace

QImage NodeContentRasterizer::rasterize(const core::Node &node,
                                        const theme::ThemeTable &theme) const
{
    const QSizeF worldSize = node.worldSize;
    const QSize pixels(qMax(1, qRound(worldSize.width() * kScale)),
                       qMax(1, qRound(worldSize.height() * kScale)));
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.scale(kScale, kScale);

    // All layout below works in world units, translated to the card's origin.
    const QPointF origin = node.worldPos;
    auto local = [&origin](const QRectF &world) {
        return world.translated(-origin);
    };

    // Header: the title, and the model chip on a generation node.
    const QRectF header = local(NodeCardLayout::headerRect(node));
    QRectF titleRect = header.adjusted(12.0, 0.0, -8.0, 0.0);
    const QRectF chip = local(NodeCardLayout::modelChipRect(node));
    if (!chip.isEmpty())
        titleRect.setRight(chip.left() - 8.0);

    painter.setFont(uiFont(13.0, QFont::DemiBold));
    painter.setPen(theme.textPrimary());
    QFontMetricsF titleMetrics(painter.font());
    painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                     titleMetrics.elidedText(node.title, Qt::ElideRight,
                                             titleRect.width()));

    if (!chip.isEmpty()) {
        painter.setFont(uiFont(11.0));
        painter.setPen(theme.textSecondary());
        const QString chipText = node.modelLabel.isEmpty()
            ? QStringLiteral("Pick a model")
            : node.modelLabel;
        QFontMetricsF chipMetrics(painter.font());
        painter.drawText(chip, Qt::AlignVCenter | Qt::AlignRight,
                         chipMetrics.elidedText(chipText, Qt::ElideMiddle,
                                                chip.width()));
    }

    const QRectF body = local(NodeCardLayout::bodyRect(node)).adjusted(12.0, 8.0,
                                                                       -12.0, -8.0);
    if (node.kind == core::NodeKind::Prompt) {
        if (node.promptText.trimmed().isEmpty()) {
            painter.setFont(uiFont(12.0));
            painter.setPen(theme.textSecondary());
            painter.drawText(body, Qt::AlignCenter | Qt::TextWordWrap,
                             QStringLiteral("Double-click to write a prompt"));
        } else {
            painter.setFont(uiFont(13.0));
            painter.setPen(theme.textPrimary());
            painter.drawText(body, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             node.promptText);
        }
    } else if (node.kind == core::NodeKind::Generate && node.resultPath.isEmpty()
               && node.runState == core::RunState::Idle) {
        painter.setFont(uiFont(12.0));
        painter.setPen(theme.textSecondary());
        painter.drawText(body, Qt::AlignCenter | Qt::TextWordWrap,
                         QStringLiteral("No result yet — press run"));
    }

    // Footer: the status dot's text partner, plus the run glyph drawn by the
    // mesh; only text lands here.
    const QRectF status = local(NodeCardLayout::statusRect(node));
    if (!status.isEmpty()) {
        const StatusLine line = statusLine(node, theme);
        const bool numeric = node.runState == core::RunState::Done;
        painter.setFont(numeric ? numericFont(11.0) : uiFont(11.0));
        painter.setPen(line.color);
        QFontMetricsF statusMetrics(painter.font());
        // Text clears the status dot drawn at the footer's left edge.
        const QRectF textRect = status.adjusted(14.0, 0.0, 0.0, 0.0);
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                         statusMetrics.elidedText(line.text, Qt::ElideRight,
                                                  textRect.width()));
    }

    painter.end();
    return image;
}

} // namespace cutpilot::render
