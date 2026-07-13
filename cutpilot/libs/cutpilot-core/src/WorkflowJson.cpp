#include "cutpilot/core/WorkflowJson.h"

#include "cutpilot/core/NodeGraph.h"

#include <QJsonArray>

#include <cmath>

namespace cutpilot::core {

namespace {

constexpr int kFormatVersion = 1;
const QLatin1String kFormatKey("cutpilot-workflow");

QString kindKey(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Blank:
        return QStringLiteral("blank");
    case NodeKind::Prompt:
        return QStringLiteral("prompt");
    case NodeKind::Generate:
        return QStringLiteral("generate");
    case NodeKind::CostGate:
        return QStringLiteral("cost-gate");
    case NodeKind::Still:
        return QStringLiteral("still");
    case NodeKind::Video:
        return QStringLiteral("video");
    case NodeKind::Blend:
        return QStringLiteral("blend");
    case NodeKind::Mask:
        return QStringLiteral("mask");
    case NodeKind::Key:
        return QStringLiteral("key");
    case NodeKind::Transform:
        return QStringLiteral("transform");
    case NodeKind::Frame:
        return QStringLiteral("frame");
    }
    return QStringLiteral("blank");
}

bool kindFromKey(const QString &key, NodeKind &kind)
{
    static const struct {
        const char *key;
        NodeKind kind;
    } table[] = {
        { "blank", NodeKind::Blank },     { "prompt", NodeKind::Prompt },
        { "generate", NodeKind::Generate }, { "cost-gate", NodeKind::CostGate },
        { "still", NodeKind::Still },     { "video", NodeKind::Video },
        { "blend", NodeKind::Blend },     { "mask", NodeKind::Mask },
        { "key", NodeKind::Key },         { "transform", NodeKind::Transform },
        { "frame", NodeKind::Frame },
    };
    for (const auto &row : table) {
        if (key == QLatin1String(row.key)) {
            kind = row.kind;
            return true;
        }
    }
    return false;
}

QString portTypeKey(PortType type)
{
    switch (type) {
    case PortType::Image:
        return QStringLiteral("image");
    case PortType::Mask:
        return QStringLiteral("mask");
    case PortType::Video:
        return QStringLiteral("video");
    case PortType::Audio:
        return QStringLiteral("audio");
    case PortType::Text:
        return QStringLiteral("text");
    case PortType::Number:
        return QStringLiteral("number");
    case PortType::Control:
        return QStringLiteral("control");
    case PortType::Any:
        break;
    }
    return QStringLiteral("any");
}

bool portTypeFromKey(const QString &key, PortType &type)
{
    static const struct {
        const char *key;
        PortType type;
    } table[] = {
        { "image", PortType::Image },   { "mask", PortType::Mask },
        { "video", PortType::Video },   { "audio", PortType::Audio },
        { "text", PortType::Text },     { "number", PortType::Number },
        { "control", PortType::Control }, { "any", PortType::Any },
    };
    for (const auto &row : table) {
        if (key == QLatin1String(row.key)) {
            type = row.type;
            return true;
        }
    }
    return false;
}

QJsonObject compToJson(const CompositeParams &p)
{
    QJsonObject json;
    json[QLatin1String("blendMode")] = int(p.blendMode);
    json[QLatin1String("opacity")] = p.opacity;
    json[QLatin1String("invertMask")] = p.invertMask;
    json[QLatin1String("lumaKey")] = p.lumaKey;
    json[QLatin1String("keyColor")] = p.keyColor.name(QColor::HexArgb);
    json[QLatin1String("keyTolerance")] = p.keyTolerance;
    json[QLatin1String("keySoftness")] = p.keySoftness;
    json[QLatin1String("translateX")] = p.translateX;
    json[QLatin1String("translateY")] = p.translateY;
    json[QLatin1String("scale")] = p.scale;
    json[QLatin1String("rotationDeg")] = p.rotationDeg;
    return json;
}

CompositeParams compFromJson(const QJsonObject &json)
{
    CompositeParams p;
    const int mode = json[QLatin1String("blendMode")].toInt(int(p.blendMode));
    if (mode >= int(BlendMode::Normal) && mode <= int(BlendMode::Add))
        p.blendMode = BlendMode(mode);
    p.opacity = json[QLatin1String("opacity")].toDouble(p.opacity);
    p.invertMask = json[QLatin1String("invertMask")].toBool(p.invertMask);
    p.lumaKey = json[QLatin1String("lumaKey")].toBool(p.lumaKey);
    const QColor keyColor(json[QLatin1String("keyColor")].toString());
    if (keyColor.isValid())
        p.keyColor = keyColor;
    p.keyTolerance = json[QLatin1String("keyTolerance")].toDouble(p.keyTolerance);
    p.keySoftness = json[QLatin1String("keySoftness")].toDouble(p.keySoftness);
    p.translateX = json[QLatin1String("translateX")].toDouble(p.translateX);
    p.translateY = json[QLatin1String("translateY")].toDouble(p.translateY);
    p.scale = json[QLatin1String("scale")].toDouble(p.scale);
    p.rotationDeg = json[QLatin1String("rotationDeg")].toDouble(p.rotationDeg);
    return p;
}

QJsonObject nodeToJson(const Node &node)
{
    QJsonObject json;
    json[QLatin1String("id")] = node.id;
    json[QLatin1String("title")] = node.title;
    json[QLatin1String("kind")] = kindKey(node.kind);
    json[QLatin1String("x")] = node.worldPos.x();
    json[QLatin1String("y")] = node.worldPos.y();
    json[QLatin1String("width")] = node.worldSize.width();
    json[QLatin1String("height")] = node.worldSize.height();

    QJsonArray ports;
    for (const Port &port : node.ports) {
        QJsonObject portJson;
        portJson[QLatin1String("name")] = port.name;
        portJson[QLatin1String("type")] = portTypeKey(port.type);
        portJson[QLatin1String("input")] = port.isInput;
        portJson[QLatin1String("edge")] = port.edgeFraction;
        ports.push_back(portJson);
    }
    json[QLatin1String("ports")] = ports;

    if (!node.promptText.isEmpty())
        json[QLatin1String("promptText")] = node.promptText;
    if (!node.modelId.isEmpty()) {
        json[QLatin1String("modelId")] = node.modelId;
        json[QLatin1String("modelLabel")] = node.modelLabel;
    }
    if (node.outputWidth > 0 && node.outputHeight > 0) {
        json[QLatin1String("outputWidth")] = node.outputWidth;
        json[QLatin1String("outputHeight")] = node.outputHeight;
    }
    json[QLatin1String("gateLimitUsd")] = node.gateLimitUsd;
    if (!node.mediaPath.isEmpty())
        json[QLatin1String("mediaPath")] = node.mediaPath;
    if (!node.externalType.isEmpty()) {
        json[QLatin1String("externalType")] = node.externalType;
        json[QLatin1String("externalData")] = node.externalData;
    }
    json[QLatin1String("comp")] = compToJson(node.comp);
    if (!node.resultPath.isEmpty()) {
        json[QLatin1String("resultPath")] = node.resultPath;
        json[QLatin1String("resultDigest")] = node.resultDigest;
        json[QLatin1String("costUsd")] = node.costUsd;
        json[QLatin1String("resultWidth")] = node.resultWidth;
        json[QLatin1String("resultHeight")] = node.resultHeight;
    }
    return json;
}

bool nodeFromJson(const QJsonObject &json, Node &node)
{
    if (!json[QLatin1String("id")].isDouble())
        return false;
    node.id = json[QLatin1String("id")].toInt();
    if (node.id <= 0)
        return false;
    if (!kindFromKey(json[QLatin1String("kind")].toString(), node.kind))
        return false;

    node.title = json[QLatin1String("title")].toString();
    node.worldPos = QPointF(json[QLatin1String("x")].toDouble(),
                            json[QLatin1String("y")].toDouble());
    node.worldSize = QSizeF(json[QLatin1String("width")].toDouble(),
                            json[QLatin1String("height")].toDouble());
    // Geometry must be finite: one infinite rect would poison the board
    // bounds, the spatial index, and the camera-fit math.
    if (!std::isfinite(node.worldPos.x()) || !std::isfinite(node.worldPos.y())
        || !std::isfinite(node.worldSize.width())
        || !std::isfinite(node.worldSize.height())
        || node.worldSize.width() < 0.0 || node.worldSize.height() < 0.0)
        return false;

    for (const QJsonValue &value : json[QLatin1String("ports")].toArray()) {
        const QJsonObject portJson = value.toObject();
        Port port;
        port.name = portJson[QLatin1String("name")].toString();
        if (!portTypeFromKey(portJson[QLatin1String("type")].toString(), port.type))
            return false;
        port.isInput = portJson[QLatin1String("input")].toBool();
        port.edgeFraction = portJson[QLatin1String("edge")].toDouble(0.5);
        node.ports.push_back(port);
    }

    node.promptText = json[QLatin1String("promptText")].toString();
    node.modelId = json[QLatin1String("modelId")].toString();
    node.modelLabel = json[QLatin1String("modelLabel")].toString();
    node.outputWidth = qMax(0, json[QLatin1String("outputWidth")].toInt());
    node.outputHeight = qMax(0, json[QLatin1String("outputHeight")].toInt());
    node.gateLimitUsd =
        json[QLatin1String("gateLimitUsd")].toDouble(node.gateLimitUsd);
    node.mediaPath = json[QLatin1String("mediaPath")].toString();
    node.externalType = json[QLatin1String("externalType")].toString();
    node.externalData = json[QLatin1String("externalData")].toString();
    node.comp = compFromJson(json[QLatin1String("comp")].toObject());
    node.resultPath = json[QLatin1String("resultPath")].toString();
    node.resultDigest = json[QLatin1String("resultDigest")].toString();
    node.costUsd = json[QLatin1String("costUsd")].toDouble(-1.0);
    node.resultWidth = json[QLatin1String("resultWidth")].toInt();
    node.resultHeight = json[QLatin1String("resultHeight")].toInt();
    return true;
}

} // namespace

QJsonObject workflowToJson(const NodeGraph &graph, const QString &name)
{
    QJsonObject json;
    json[kFormatKey] = kFormatVersion;
    json[QLatin1String("name")] = name;

    QJsonArray nodes;
    for (const Node &node : graph.nodes())
        nodes.push_back(nodeToJson(node));
    json[QLatin1String("nodes")] = nodes;

    QJsonArray connections;
    for (const Connection &connection : graph.connections()) {
        QJsonObject wire;
        wire[QLatin1String("id")] = connection.id;
        wire[QLatin1String("from")] = connection.fromNodeId;
        wire[QLatin1String("fromPort")] = connection.fromPortIndex;
        wire[QLatin1String("to")] = connection.toNodeId;
        wire[QLatin1String("toPort")] = connection.toPortIndex;
        connections.push_back(wire);
    }
    json[QLatin1String("connections")] = connections;
    return json;
}

bool workflowFromJson(const QJsonObject &json, NodeGraph &graph, QString *name)
{
    if (json[kFormatKey].toInt() != kFormatVersion)
        return false;
    if (!graph.nodes().isEmpty() || !graph.connections().isEmpty())
        return false;

    // Stage everything first so a malformed entry rejects the whole document
    // instead of leaving the graph half-populated.
    QVector<Node> nodes;
    for (const QJsonValue &value : json[QLatin1String("nodes")].toArray()) {
        Node node;
        if (!value.isObject() || !nodeFromJson(value.toObject(), node))
            return false;
        for (const Node &existing : nodes) {
            if (existing.id == node.id)
                return false;
        }
        nodes.push_back(node);
    }

    const auto endpointValid = [&nodes](int nodeId, int portIndex) {
        for (const Node &node : nodes) {
            if (node.id == nodeId)
                return portIndex >= 0 && portIndex < node.ports.size();
        }
        return false;
    };

    QVector<Connection> connections;
    for (const QJsonValue &value : json[QLatin1String("connections")].toArray()) {
        if (!value.isObject())
            return false;
        const QJsonObject wire = value.toObject();
        Connection connection;
        connection.id = wire[QLatin1String("id")].toInt();
        connection.fromNodeId = wire[QLatin1String("from")].toInt();
        connection.fromPortIndex = wire[QLatin1String("fromPort")].toInt();
        connection.toNodeId = wire[QLatin1String("to")].toInt();
        connection.toPortIndex = wire[QLatin1String("toPort")].toInt();
        if (connection.id <= 0
            || !endpointValid(connection.fromNodeId, connection.fromPortIndex)
            || !endpointValid(connection.toNodeId, connection.toPortIndex))
            return false;
        for (const Connection &existing : connections) {
            if (existing.id == connection.id)
                return false;
        }
        connections.push_back(connection);
    }

    for (const Node &node : nodes)
        graph.insertNode(graph.nodes().size(), node);
    for (const Connection &connection : connections)
        graph.insertConnection(graph.connections().size(), connection);

    if (name)
        *name = json[QLatin1String("name")].toString();
    return true;
}

} // namespace cutpilot::core
