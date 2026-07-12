#include "cutpilot/core/CompositeNodes.h"

namespace cutpilot::core {

Node compositeNodePrototype(NodeKind kind)
{
    Node node;
    node.kind = kind;

    switch (kind) {
    case NodeKind::Still:
        node.title = QStringLiteral("Still Image");
        node.worldSize = QSizeF(260.0, 180.0);
        node.ports = {
            { QStringLiteral("image"), PortType::Image, false, 0.5 },
        };
        break;
    case NodeKind::Video:
        node.title = QStringLiteral("Video");
        node.worldSize = QSizeF(280.0, 190.0);
        node.ports = {
            { QStringLiteral("frame"), PortType::Image, false, 0.5 },
        };
        break;
    case NodeKind::Blend:
        node.title = QStringLiteral("Blend");
        node.worldSize = QSizeF(260.0, 190.0);
        node.ports = {
            { QStringLiteral("base"), PortType::Image, true, 0.3 },
            { QStringLiteral("over"), PortType::Image, true, 0.6 },
            { QStringLiteral("result"), PortType::Image, false, 0.5 },
        };
        break;
    case NodeKind::Mask:
        node.title = QStringLiteral("Mask");
        node.worldSize = QSizeF(260.0, 190.0);
        node.ports = {
            { QStringLiteral("image"), PortType::Image, true, 0.3 },
            { QStringLiteral("mask"), PortType::Mask, true, 0.6 },
            { QStringLiteral("result"), PortType::Image, false, 0.5 },
        };
        break;
    case NodeKind::Key:
        node.title = QStringLiteral("Key");
        node.worldSize = QSizeF(260.0, 190.0);
        node.ports = {
            { QStringLiteral("image"), PortType::Image, true, 0.5 },
            { QStringLiteral("keyed"), PortType::Image, false, 0.35 },
            { QStringLiteral("matte"), PortType::Mask, false, 0.65 },
        };
        break;
    case NodeKind::Transform:
        node.title = QStringLiteral("Transform");
        node.worldSize = QSizeF(260.0, 190.0);
        node.ports = {
            { QStringLiteral("image"), PortType::Image, true, 0.5 },
            { QStringLiteral("result"), PortType::Image, false, 0.5 },
        };
        break;
    default:
        node.kind = NodeKind::Blank;
        node.title = QStringLiteral("Node");
        node.worldSize = QSizeF(220.0, 140.0);
        break;
    }
    return node;
}

int blendModeCount()
{
    return 5;
}

QString blendModeLabel(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Normal:
        return QStringLiteral("Normal");
    case BlendMode::Multiply:
        return QStringLiteral("Multiply");
    case BlendMode::Screen:
        return QStringLiteral("Screen");
    case BlendMode::Overlay:
        return QStringLiteral("Overlay");
    case BlendMode::Add:
        return QStringLiteral("Add");
    }
    return QString();
}

} // namespace cutpilot::core
