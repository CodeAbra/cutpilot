#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace cutpilot::core {

// The kind of data a port carries. Drives the typed-port color, the port's shape
// (data ports are round, control ports square), and connection compatibility.
enum class PortType {
    Image,
    Mask,
    Video,
    Audio,
    Text,
    Number,
    Control,
    Any
};

// A single typed connection point on a node edge. Inputs sit on the left edge,
// outputs on the right; the fractional offset places the dot along that edge
// (0 at the top, 1 at the bottom) so a node can carry several ports per side.
struct Port {
    QString name;
    PortType type = PortType::Any;
    bool isInput = true;
    qreal edgeFraction = 0.5;
};

// A node on the canvas, expressed entirely in world coordinates. The body is the
// content; the header is a slim strip carrying the title and model name. The
// renderer draws this; the node never becomes a per-instance widget.
struct Node {
    int id = 0;
    QString title;
    QPointF worldPos{0, 0};   // top-left of the card, in world units
    QSizeF worldSize{0, 0};   // card size, in world units
    bool selected = false;
    QVector<Port> ports;

    QRectF worldRect() const { return QRectF(worldPos, worldSize); }
    bool containsWorld(const QPointF &world) const { return worldRect().contains(world); }

    // The world-space center of a port: on the left edge for an input, the right
    // edge for an output, at the port's fractional offset along that edge.
    QPointF portWorldPosition(int portIndex) const
    {
        const Port &port = ports.at(portIndex);
        const QRectF rect = worldRect();
        const qreal x = port.isInput ? rect.left() : rect.right();
        const qreal y = rect.top() + rect.height() * port.edgeFraction;
        return QPointF(x, y);
    }

    // The port whose center lies within radius of the world point, or -1. When
    // several qualify the nearest wins.
    int portIndexAtWorld(const QPointF &world, qreal radius) const
    {
        int best = -1;
        qreal bestDistSq = radius * radius;
        for (int i = 0; i < ports.size(); ++i) {
            const QPointF d = portWorldPosition(i) - world;
            const qreal distSq = d.x() * d.x() + d.y() * d.y();
            if (distSq <= bestDistSq) {
                bestDistSq = distSq;
                best = i;
            }
        }
        return best;
    }
};

} // namespace cutpilot::core
