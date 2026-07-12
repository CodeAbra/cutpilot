#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include <cstdint>

namespace cutpilot::core {
struct Node;
}

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::render {

// Builds the triangulated, vertex-colored geometry for the node layer in the item's
// logical pixel space. Every node — body, slim header, border, typed port dots, and
// the selection outline — is emitted as colored triangles into one shared
// vertex/index buffer, so the whole layer is one batchable scene-graph geometry
// rather than a widget per node.
//
// The mesh is held as plain arrays here and copied into a QSGGeometry by the layer;
// keeping the math out of the scene-graph node keeps this unit-checkable and free of
// QtQuick dependencies.
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

    // Clear and rebuild the mesh for all nodes at the given camera. worldToScreen
    // maps a world point to the layer's logical pixel coordinates; zoom is the
    // world-to-screen scale (logical), used for level-of-detail and to size strokes
    // and ports in world-proportional terms.
    void build(const QVector<core::Node> &nodes,
               const theme::ThemeTable &theme,
               qreal zoom,
               const QPointF &panLogical);

    const QVector<Vertex> &vertices() const { return m_vertices; }
    const QVector<uint16_t> &indices() const { return m_indices; }

private:
    QPointF toScreen(const QPointF &world, qreal zoom, const QPointF &panLogical) const;

    void appendRoundedRect(const QRectF &rect, qreal radius, const QColor &color);
    void appendRoundedRectStroke(const QRectF &rect, qreal radius, qreal width,
                                 const QColor &color);
    void appendDisc(const QPointF &center, qreal radius, const QColor &color);

    void appendTriangle(const QPointF &a, const QPointF &b, const QPointF &c,
                        const QColor &color);

    QVector<Vertex> m_vertices;
    QVector<uint16_t> m_indices;
};

} // namespace cutpilot::render
