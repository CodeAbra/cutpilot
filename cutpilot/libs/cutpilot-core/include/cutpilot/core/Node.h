#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace cutpilot::core {

// The kind of data a port carries. Drives the typed-port color and, in later
// phases, connection-compatibility rules.
enum class PortType {
    Image,
    Video,
    Audio,
    Text,
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
};

} // namespace cutpilot::core
