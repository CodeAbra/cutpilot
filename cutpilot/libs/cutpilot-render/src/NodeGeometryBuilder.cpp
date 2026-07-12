#include "NodeGeometryBuilder.h"
#include "NodeCardLayout.h"

#include "cutpilot/core/Node.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QtGlobal>
#include <QtMath>

#include <array>

namespace cutpilot::render {

namespace {

// Node card geometry in world units: a slim header strip over a larger body,
// rounded corners, small typed ports on the side edges (round for data, square for
// control), a hairline border, and the selection outline and elevation halo of a
// selected card.
constexpr qreal kBodyRadiusWorld = 10.0;
constexpr qreal kBorderWorldWidth = 1.5;
constexpr qreal kPortBackingWorldWidth = 1.0;
constexpr qreal kSelectionWorldWidth = 2.0;
constexpr qreal kHaloWorldWidth = 6.0;

// Segments per 90-degree corner. Eight reads as round at the sizes a node occupies
// on screen without inflating the vertex count.
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

} // namespace

QColor NodeGeometryBuilder::portColor(core::PortType type, const theme::ThemeTable &theme)
{
    switch (type) {
    case core::PortType::Image:
        return theme.typeImage();
    case core::PortType::Mask:
        return theme.typeMask();
    case core::PortType::Video:
        return theme.typeVideo();
    case core::PortType::Audio:
        return theme.typeAudio();
    case core::PortType::Text:
        return theme.typeText();
    case core::PortType::Number:
        return theme.typeNumber();
    case core::PortType::Control:
        return theme.typeControl();
    case core::PortType::Any:
        break;
    }
    return theme.typeAny();
}

void NodeGeometryBuilder::appendTriangle(Mesh &mesh, const QPointF &a, const QPointF &b,
                                         const QPointF &c, const QColor &color)
{
    const uint8_t r = toByte(color.redF());
    const uint8_t g = toByte(color.greenF());
    const uint8_t bl = toByte(color.blueF());
    const uint8_t al = toByte(color.alphaF());

    const auto base = static_cast<uint16_t>(mesh.vertices.size());
    for (const QPointF &p : { a, b, c })
        mesh.vertices.push_back({ float(p.x()), float(p.y()), r, g, bl, al });
    mesh.indices.push_back(base);
    mesh.indices.push_back(static_cast<uint16_t>(base + 1));
    mesh.indices.push_back(static_cast<uint16_t>(base + 2));
}

void NodeGeometryBuilder::appendQuad(Mesh &mesh, const QRectF &rect, const QColor &color)
{
    appendTriangle(mesh, rect.topLeft(), rect.topRight(), rect.bottomRight(), color);
    appendTriangle(mesh, rect.topLeft(), rect.bottomRight(), rect.bottomLeft(), color);
}

void NodeGeometryBuilder::appendLineQuad(Mesh &mesh, const QPointF &a, const QPointF &b,
                                         qreal width, const QColor &color)
{
    QPointF direction = b - a;
    const qreal length = std::hypot(direction.x(), direction.y());
    if (length <= 1e-6)
        return;
    direction /= length;
    const QPointF normal(-direction.y(), direction.x());
    const QPointF offset = normal * (width / 2.0);
    appendTriangle(mesh, a + offset, b + offset, b - offset, color);
    appendTriangle(mesh, a + offset, b - offset, a - offset, color);
}

// A filled rounded rect, triangulated as a fan from the rect center out to a ring of
// boundary points (straight edges plus tessellated corner arcs).
void NodeGeometryBuilder::appendRoundedRect(Mesh &mesh, const QRectF &rect, qreal radius,
                                            const QColor &color)
{
    const qreal r = qMin(radius, qMin(rect.width(), rect.height()) / 2.0);
    if (r <= 0.5) {
        // Degenerate to a plain quad.
        appendTriangle(mesh, rect.topLeft(), rect.topRight(), rect.bottomRight(), color);
        appendTriangle(mesh, rect.topLeft(), rect.bottomRight(), rect.bottomLeft(), color);
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
    // Each arc sweeps 90 degrees; start angles run clockwise in a y-down frame, so
    // the boundary is generated in a consistent winding.
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
        appendTriangle(mesh, center, p0, p1, color);
    }
}

// A stroked rounded-rect outline of the given width, built as the band between an
// outer and an inner rounded rect. Emitted as quads following the boundary ring.
void NodeGeometryBuilder::appendRoundedRectStroke(Mesh &mesh, const QRectF &rect,
                                                  qreal radius, qreal width,
                                                  const QColor &color)
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
        appendTriangle(mesh, outerRing[i], outerRing[j], innerRing[i], color);
        appendTriangle(mesh, innerRing[i], outerRing[j], innerRing[j], color);
    }
}

void NodeGeometryBuilder::appendDisc(Mesh &mesh, const QPointF &center, qreal radius,
                                     const QColor &color)
{
    const int segments = qMax(10, kCornerSegments * 2);
    for (int i = 0; i < segments; ++i) {
        const qreal a0 = (qreal(i) / segments) * 2.0 * M_PI;
        const qreal a1 = (qreal(i + 1) / segments) * 2.0 * M_PI;
        const QPointF p0 = center + QPointF(radius * std::cos(a0), radius * std::sin(a0));
        const QPointF p1 = center + QPointF(radius * std::cos(a1), radius * std::sin(a1));
        appendTriangle(mesh, center, p0, p1, color);
    }
}

NodeGeometryBuilder::Mesh
NodeGeometryBuilder::buildNode(const core::Node &node, const theme::ThemeTable &theme,
                               bool detailed) const
{
    Mesh mesh;

    const QColor canvas = theme.bgCanvas();
    const QRectF rect = node.worldRect();
    const qreal radius = kBodyRadiusWorld;

    // Resting card: a rounded body with a hairline border. The selected node first
    // lays down a neutral halo ring just outside the card (the elevation lift), then
    // the selection outline replaces the resting border.
    if (node.selected) {
        appendRoundedRectStroke(mesh, rect, radius, kHaloWorldWidth,
                                over(theme.glowEmphasis(), canvas));
    }

    appendRoundedRect(mesh, rect, radius, theme.nodeBody());

    if (detailed) {
        // Slim header strip over the top of the body, its bottom corners squared so
        // only the top of the card rounds.
        QRectF headerRect = rect;
        headerRect.setHeight(qMin(NodeCardLayout::kHeaderHeight, rect.height()));
        appendRoundedRect(mesh, headerRect, radius, theme.nodeHeader());
        const qreal seam = radius;
        if (headerRect.height() > seam) {
            const QRectF lower(headerRect.left(), headerRect.bottom() - seam,
                               headerRect.width(), seam);
            appendRoundedRect(mesh,
                              QRectF(lower.left(), lower.top(), lower.width(),
                                     qMin(seam, headerRect.height())),
                              0.0, theme.nodeHeader());
        }

        // A generation node adds a media well and a footer: progress band,
        // status dot, and the run control. The well sits between header and
        // footer, so its corners are square and safe to fill edge to edge.
        const QRectF footer = NodeCardLayout::footerRect(node);
        if (!footer.isEmpty()) {
            appendQuad(mesh, NodeCardLayout::bodyRect(node), theme.bgCanvas());

            // Footer strip mirrors the header: rounded at the card's bottom,
            // squared along its top seam.
            appendRoundedRect(mesh, footer, radius, theme.nodeHeader());
            if (footer.height() > seam)
                appendQuad(mesh,
                           QRectF(footer.left(), footer.top(), footer.width(), seam),
                           theme.nodeHeader());

            const bool inFlight = node.runState == core::RunState::Queued
                || node.runState == core::RunState::Running;
            if (inFlight) {
                const QRectF track = NodeCardLayout::progressRect(node);
                appendQuad(mesh, track, theme.borderSubtle());
                if (node.runProgress > 0.0) {
                    QRectF fill = track;
                    fill.setWidth(track.width()
                                  * qBound(0.0, node.runProgress, 1.0));
                    appendQuad(mesh, fill, theme.statusRunning());
                }
            }

            QColor dot = theme.borderDefault();
            switch (node.runState) {
            case core::RunState::Queued:
            case core::RunState::Running:
                dot = theme.statusRunning();
                break;
            case core::RunState::Done:
                dot = theme.statusDone();
                break;
            case core::RunState::Error:
                dot = theme.statusError();
                break;
            case core::RunState::NeedsKey:
                dot = theme.statusWarning();
                break;
            case core::RunState::Idle:
                break;
            }
            const qreal dotY =
                (footer.top() + NodeCardLayout::kProgressHeight + footer.bottom())
                / 2.0;
            appendDisc(mesh, QPointF(footer.left() + 16.0, dotY), 4.0, dot);

            // The run control: a resting pill with a play triangle, swapped
            // for a stop square while a job is in flight.
            const QRectF button = NodeCardLayout::runButtonRect(node);
            appendRoundedRect(mesh, button, 6.0,
                              over(theme.glowEmphasis(), theme.nodeHeader()));
            const QPointF centre = button.center();
            if (inFlight) {
                const qreal half = 5.0;
                appendQuad(mesh,
                           QRectF(centre - QPointF(half, half),
                                  QSizeF(half * 2.0, half * 2.0)),
                           theme.statusError());
            } else {
                appendTriangle(mesh, centre + QPointF(-4.0, -6.5),
                               centre + QPointF(-4.0, 6.5), centre + QPointF(7.5, 0.0),
                               theme.statusRunning());
            }
        }
    }

    // Border or selection outline.
    if (node.selected) {
        appendRoundedRectStroke(mesh, rect, radius, kSelectionWorldWidth,
                                theme.selection());
    } else {
        appendRoundedRectStroke(mesh, rect, radius, kBorderWorldWidth,
                                theme.borderDefault());
    }

    // Typed ports on the side edges, hidden at the low-detail tier. Data ports are
    // round and control ports square, so the type system never leans on color alone.
    if (detailed) {
        for (int i = 0; i < node.ports.size(); ++i) {
            const core::Port &port = node.ports[i];
            const QPointF center = node.portWorldPosition(i);
            const QColor color = portColor(port.type, theme);
            // A subtle dark backing separates the port from the card edge, then the
            // typed color on top.
            if (port.type == core::PortType::Control) {
                const qreal outer = kPortRadiusWorld + kPortBackingWorldWidth;
                appendQuad(mesh,
                           QRectF(center - QPointF(outer, outer),
                                  QSizeF(outer * 2.0, outer * 2.0)),
                           theme.bgCanvas());
                appendQuad(mesh,
                           QRectF(center - QPointF(kPortRadiusWorld, kPortRadiusWorld),
                                  QSizeF(kPortRadiusWorld * 2.0, kPortRadiusWorld * 2.0)),
                           color);
            } else {
                appendDisc(mesh, center, kPortRadiusWorld + kPortBackingWorldWidth,
                           theme.bgCanvas());
                appendDisc(mesh, center, kPortRadiusWorld, color);
            }
        }
    }

    // One node's mesh stays well under the 16-bit index ceiling; a node that grew
    // past it would need splitting into multiple geometry nodes.
    Q_ASSERT(mesh.vertices.size() <= 0xFFFF);
    return mesh;
}

NodeGeometryBuilder::Mesh
NodeGeometryBuilder::buildScreenRect(const QRectF &rect, const QColor &fill,
                                     const QColor &outline, qreal outlineWidth) const
{
    Mesh mesh;
    const QRectF r = rect.normalized();

    appendQuad(mesh, r, fill);

    // Four edge bands tiling the outline without overlap, centred on the rect edges.
    const qreal w = qMax(outlineWidth, 0.0);
    if (w > 0.0) {
        const qreal h = w / 2.0;
        const QRectF outer = r.adjusted(-h, -h, h, h);
        appendQuad(mesh, QRectF(outer.left(), outer.top(), outer.width(), w), outline);
        appendQuad(mesh, QRectF(outer.left(), outer.bottom() - w, outer.width(), w),
                   outline);
        appendQuad(mesh, QRectF(outer.left(), outer.top() + w, w, outer.height() - 2 * w),
                   outline);
        appendQuad(mesh,
                   QRectF(outer.right() - w, outer.top() + w, w, outer.height() - 2 * w),
                   outline);
    }

    Q_ASSERT(mesh.vertices.size() <= 0xFFFF);
    return mesh;
}

NodeGeometryBuilder::Mesh
NodeGeometryBuilder::buildScreenLines(const QVector<QLineF> &lines, const QColor &color,
                                      qreal width) const
{
    Mesh mesh;
    for (const QLineF &line : lines)
        appendLineQuad(mesh, line.p1(), line.p2(), width, color);
    Q_ASSERT(mesh.vertices.size() <= 0xFFFF);
    return mesh;
}

} // namespace cutpilot::render
