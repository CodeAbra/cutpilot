#pragma once

#include <QColor>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include <cstdint>

namespace cutpilot::core {
struct Node;
enum class PortType;
}

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::render {

// Builds the triangulated, vertex-colored mesh for one node in world coordinates:
// body, slim header, border, typed port dots, and the selection outline, all as
// colored triangles. Vertices are world-space, so pan and zoom are applied by the
// layer's scene-graph transform and never re-triangulate the mesh. Each node is
// meshed on its own, so its indices are node-local (never near the 16-bit index
// ceiling) and the layer emits one scene-graph geometry node per node — the per-node
// batching and culling granularity a hundreds-of-nodes board depends on.
//
// The mesh is plain arrays here and copied into a QSGGeometry by the layer; keeping
// the math out of the scene-graph node keeps it unit-checkable and free of QtQuick
// dependencies.
class NodeGeometryBuilder {
public:
    struct Vertex {
        float x;
        float y;
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    struct Mesh {
        QVector<Vertex> vertices;
        QVector<uint16_t> indices;
    };

    // On-screen zoom at or above which a node draws its header and ports; below it
    // the card drops to a single solid rounded body (the low-zoom detail tier).
    static constexpr qreal kDetailZoom = 0.45;

    // Build one node's world-space mesh. detailed selects the level-of-detail tier.
    Mesh buildNode(const core::Node &node, const theme::ThemeTable &theme,
                   bool detailed) const;

    // Build a screen-space rectangle as a translucent fill under a thin outline. Used
    // for the marquee band; the rect is in the item's logical pixels so it draws at a
    // constant width under the transform-free overlay root regardless of zoom.
    Mesh buildScreenRect(const QRectF &rect, const QColor &fill, const QColor &outline,
                         qreal outlineWidth) const;

    // Build a set of screen-space lines as thin quads of the given width. Used for the
    // alignment guides; the lines are in the item's logical pixels, so a guide stays a
    // constant hairline at any zoom.
    Mesh buildScreenLines(const QVector<QLineF> &lines, const QColor &color,
                          qreal width) const;

    // The resolved theme color for a port type — shared by the port dots, the
    // connectors, and the live wiring drag so a type reads as one hue everywhere.
    static QColor portColor(core::PortType type, const theme::ThemeTable &theme);

    // The world radius of a port dot (control ports draw as squares of the same
    // reach). Shared with hit-testing so the visual and the target agree.
    static constexpr qreal kPortRadiusWorld = 5.0;

    // Shared mesh primitives, reused by the connector builder.
    static void appendQuad(Mesh &mesh, const QRectF &rect, const QColor &color);
    static void appendLineQuad(Mesh &mesh, const QPointF &a, const QPointF &b,
                               qreal width, const QColor &color);
    static void appendDisc(Mesh &mesh, const QPointF &center, qreal radius,
                           const QColor &color);

private:
    static void appendTriangle(Mesh &mesh, const QPointF &a, const QPointF &b,
                               const QPointF &c, const QColor &color);
    static void appendRoundedRect(Mesh &mesh, const QRectF &rect, qreal radius,
                                  const QColor &color);
    static void appendRoundedRectStroke(Mesh &mesh, const QRectF &rect, qreal radius,
                                        qreal width, const QColor &color);
};

} // namespace cutpilot::render
