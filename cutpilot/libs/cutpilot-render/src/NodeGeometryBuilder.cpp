#include "NodeGeometryBuilder.h"

#include "cutpilot/core/Node.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QtMath>

#include <array>

namespace cutpilot::render {

namespace {

// Node card geometry in world units. These are the resting design proportions of a
// content-first card: a slim header strip over a larger body, rounded corners, and
// small typed port dots on the side edges.
constexpr qreal kHeaderWorldHeight = 30.0;
constexpr qreal kBodyRadiusWorld = 10.0;
constexpr qreal kBorderWorldWidth = 1.5;
constexpr qreal kPortWorldRadius = 5.0;
constexpr qreal kSelectionWorldWidth = 2.0;

// Level-of-detail: below this on-screen zoom the card drops its header and ports and
// reads as a single solid rounded card, matching the design's low-zoom tier.
constexpr qreal kDetailZoom = 0.45;

// Arc tessellation: segments per 90-degree corner. Eight reads as round at the
// sizes a node occupies on screen without inflating the vertex count.
constexpr int kCornerSegments = 8;

uint8_t toByte(qreal channel)
{
    return static_cast<uint8_t>(qBound(0, qRound(channel * 255.0), 255));
}

// Blend a translucent color over an opaque backdrop so a low-alpha token (the
// selection halo) resolves to an opaque vertex color rather than relying on the
// node layer's own blending, which would bleed the grid through the card.
QColor over(const QColor &top, const QColor &under)
{
    const qreal a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + under.redF() * (1.0 - a),
                            top.greenF() * a + under.greenF() * (1.0 - a),
                            top.blueF() * a + under.blueF() * (1.0 - a),
                            1.0);
}

QColor portColorFor(core::PortType type, const theme::ThemeTable &theme)
{
    switch (type) {
    case core::PortType::Image:
        return theme.typeImage();
    case core::PortType::Video:
        return theme.typeVideo();
    case core::PortType::Audio:
        return theme.typeAudio();
    case core::PortType::Text:
        return theme.typeText();
    case core::PortType::Control:
        return theme.typeControl();
    case core::PortType::Any:
        break;
    }
    return theme.typeAny();
}

} // namespace

QPointF NodeGeometryBuilder::toScreen(const QPointF &world, qreal zoom,
                                      const QPointF &panLogical) const
{
    return QPointF(world.x() * zoom + panLogical.x(),
                   world.y() * zoom + panLogical.y());
}

void NodeGeometryBuilder::appendTriangle(const QPointF &a, const QPointF &b,
                                         const QPointF &c, const QColor &color)
{
    const uint8_t r = toByte(color.redF());
    const uint8_t g = toByte(color.greenF());
    const uint8_t bl = toByte(color.blueF());
    const uint8_t al = toByte(color.alphaF());

    const auto base = static_cast<uint16_t>(m_vertices.size());
    for (const QPointF &p : { a, b, c })
        m_vertices.push_back({ float(p.x()), float(p.y()), r, g, bl, al });
    m_indices.push_back(base);
    m_indices.push_back(static_cast<uint16_t>(base + 1));
    m_indices.push_back(static_cast<uint16_t>(base + 2));
}

// A filled rounded rect, triangulated as a fan from the rect center out to a ring of
// boundary points (straight edges plus tessellated corner arcs).
void NodeGeometryBuilder::appendRoundedRect(const QRectF &rect, qreal radius,
                                            const QColor &color)
{
    const qreal r = qMin(radius, qMin(rect.width(), rect.height()) / 2.0);
    if (r <= 0.5) {
        // Degenerate to a plain quad.
        appendTriangle(rect.topLeft(), rect.topRight(), rect.bottomRight(), color);
        appendTriangle(rect.topLeft(), rect.bottomRight(), rect.bottomLeft(), color);
        return;
    }

    const QPointF center = rect.center();

    // Corner arc centers, clockwise from top-right.
    const std::array<QPointF, 4> arcCenter = {
        QPointF(rect.right() - r, rect.top() + r),    // top-right
        QPointF(rect.right() - r, rect.bottom() - r), // bottom-right
        QPointF(rect.left() + r, rect.bottom() - r),  // bottom-left
        QPointF(rect.left() + r, rect.top() + r),     // top-left
    };
    // Each arc sweeps 90 degrees; start angles run clockwise in screen space (y
    // down), so the boundary is generated in a consistent winding.
    const std::array<qreal, 4> startAngle = { -M_PI_2, 0.0, M_PI_2, M_PI };

    QVector<QPointF> boundary;
    boundary.reserve(4 * (kCornerSegments + 1));
    for (int corner = 0; corner < 4; ++corner) {
        for (int s = 0; s <= kCornerSegments; ++s) {
            const qreal t = qreal(s) / qreal(kCornerSegments);
            const qreal angle = startAngle[corner] + t * M_PI_2;
            boundary.push_back(arcCenter[corner]
                               + QPointF(r * std::cos(angle), r * std::sin(angle)));
        }
    }

    for (int i = 0; i < boundary.size(); ++i) {
        const QPointF &p0 = boundary[i];
        const QPointF &p1 = boundary[(i + 1) % boundary.size()];
        appendTriangle(center, p0, p1, color);
    }
}

// A stroked rounded-rect outline of the given width, built as the band between an
// outer and an inner rounded rect. Emitted as quads following the boundary ring.
void NodeGeometryBuilder::appendRoundedRectStroke(const QRectF &rect, qreal radius,
                                                  qreal width, const QColor &color)
{
    const qreal half = width / 2.0;
    const QRectF outer = rect.adjusted(-half, -half, half, half);
    const QRectF inner = rect.adjusted(half, half, -half, -half);
    const qreal outerR = qMax(0.0, radius + half);
    const qreal innerR = qMax(0.0, radius - half);

    auto ring = [](const QRectF &rc, qreal r) {
        const qreal rr = qMin(r, qMin(rc.width(), rc.height()) / 2.0);
        const std::array<QPointF, 4> arcCenter = {
            QPointF(rc.right() - rr, rc.top() + rr),
            QPointF(rc.right() - rr, rc.bottom() - rr),
            QPointF(rc.left() + rr, rc.bottom() - rr),
            QPointF(rc.left() + rr, rc.top() + rr),
        };
        const std::array<qreal, 4> startAngle = { -M_PI_2, 0.0, M_PI_2, M_PI };
        QVector<QPointF> pts;
        pts.reserve(4 * (kCornerSegments + 1));
        for (int corner = 0; corner < 4; ++corner) {
            for (int s = 0; s <= kCornerSegments; ++s) {
                const qreal t = qreal(s) / qreal(kCornerSegments);
                const qreal angle = startAngle[corner] + t * M_PI_2;
                pts.push_back(arcCenter[corner]
                              + QPointF(rr * std::cos(angle), rr * std::sin(angle)));
            }
        }
        return pts;
    };

    const QVector<QPointF> outerRing = ring(outer, outerR);
    const QVector<QPointF> innerRing = ring(inner, innerR);
    const int count = outerRing.size();

    for (int i = 0; i < count; ++i) {
        const int j = (i + 1) % count;
        appendTriangle(outerRing[i], outerRing[j], innerRing[i], color);
        appendTriangle(innerRing[i], outerRing[j], innerRing[j], color);
    }
}

void NodeGeometryBuilder::appendDisc(const QPointF &center, qreal radius,
                                     const QColor &color)
{
    const int segments = qMax(10, kCornerSegments * 2);
    for (int i = 0; i < segments; ++i) {
        const qreal a0 = (qreal(i) / segments) * 2.0 * M_PI;
        const qreal a1 = (qreal(i + 1) / segments) * 2.0 * M_PI;
        const QPointF p0 = center + QPointF(radius * std::cos(a0), radius * std::sin(a0));
        const QPointF p1 = center + QPointF(radius * std::cos(a1), radius * std::sin(a1));
        appendTriangle(center, p0, p1, color);
    }
}

void NodeGeometryBuilder::build(const QVector<core::Node> &nodes,
                                const theme::ThemeTable &theme, qreal zoom,
                                const QPointF &panLogical)
{
    m_vertices.clear();
    m_indices.clear();

    const QColor canvas = theme.bgCanvas();
    const bool detailed = zoom >= kDetailZoom;

    for (const core::Node &node : nodes) {
        const QRectF worldRect = node.worldRect();
        const QPointF tl = toScreen(worldRect.topLeft(), zoom, panLogical);
        const QPointF br = toScreen(worldRect.bottomRight(), zoom, panLogical);
        const QRectF screenRect(tl, br);

        const qreal radius = kBodyRadiusWorld * zoom;
        const qreal borderWidth = qMax(1.0, kBorderWorldWidth * zoom);

        // Resting card: a rounded body with a hairline border. The selected node
        // first lays down a neutral halo ring just outside the card (the elevation
        // lift), then the 2px selection outline replaces the resting border.
        if (node.selected) {
            const qreal haloWidth = qMax(3.0, 6.0 * zoom);
            appendRoundedRectStroke(screenRect, radius, haloWidth,
                                    over(theme.glowEmphasis(), canvas));
        }

        appendRoundedRect(screenRect, radius, theme.nodeBody());

        if (detailed) {
            // Slim header strip over the top of the body, clipped to the body's
            // rounded top corners by reusing the body radius on its top edge only.
            const qreal headerHeight = kHeaderWorldHeight * zoom;
            QRectF headerRect = screenRect;
            headerRect.setHeight(qMin(headerHeight, screenRect.height()));
            // Square the header's bottom corners: draw a rounded rect for the strip,
            // then a plain quad fills the seam to the body so the corner only rounds
            // at the very top of the card.
            appendRoundedRect(headerRect, radius, theme.nodeHeader());
            const qreal seam = radius;
            if (headerRect.height() > seam) {
                const QRectF lower(headerRect.left(), headerRect.bottom() - seam,
                                   headerRect.width(), seam);
                appendRoundedRect(QRectF(lower.left(), lower.top(), lower.width(),
                                         qMin(seam, headerRect.height())),
                                  0.0, theme.nodeHeader());
            }
        }

        // Border or selection outline.
        if (node.selected) {
            appendRoundedRectStroke(screenRect, radius, qMax(2.0, kSelectionWorldWidth),
                                    theme.selection());
        } else {
            appendRoundedRectStroke(screenRect, radius, borderWidth,
                                    theme.borderDefault());
        }

        // Typed port dots on the side edges, hidden at the low-detail tier.
        if (detailed) {
            const qreal portRadius = qMax(3.0, kPortWorldRadius * zoom);
            for (const core::Port &port : node.ports) {
                const qreal edgeX = port.isInput ? worldRect.left() : worldRect.right();
                const qreal edgeY = worldRect.top()
                    + worldRect.height() * port.edgeFraction;
                const QPointF centerScreen =
                    toScreen(QPointF(edgeX, edgeY), zoom, panLogical);
                // A subtle dark backing disc separates the dot from the card edge,
                // then the typed color on top.
                appendDisc(centerScreen, portRadius + qMax(1.0, zoom),
                           theme.bgCanvas());
                appendDisc(centerScreen, portRadius, portColorFor(port.type, theme));
            }
        }
    }
}

} // namespace cutpilot::render
