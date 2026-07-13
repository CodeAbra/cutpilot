#include "cutpilot/core/NodeCatalog.h"

#include "cutpilot/core/CompositeNodes.h"

namespace cutpilot::core {

namespace {

Node entry(const QString &title, const QSizeF &size, const QVector<Port> &ports,
           NodeKind kind = NodeKind::Blank, const QString &modelId = QString(),
           const QString &modelLabel = QString())
{
    Node node;
    node.kind = kind;
    node.title = title;
    node.worldSize = size;
    node.ports = ports;
    node.modelId = modelId;
    node.modelLabel = modelLabel;
    return node;
}

QVector<CatalogEntry> buildCatalog()
{
    const QString text = QStringLiteral("Text");
    const QString image = QStringLiteral("Image");
    const QString video = QStringLiteral("Video");
    const QString audio = QStringLiteral("Audio");
    const QString media = QStringLiteral("Media");
    const QString compositing = QStringLiteral("Compositing");
    const QString control = QStringLiteral("Control");
    const QString layout = QStringLiteral("Layout");

    Node frame;
    frame.kind = NodeKind::Frame;
    frame.title = QStringLiteral("Frame");
    frame.worldSize = QSizeF(620.0, 420.0);

    return {
        { text,
          entry(QStringLiteral("Prompt"), QSizeF(260, 170),
                { { QStringLiteral("text"), PortType::Text, false, 0.5 } },
                NodeKind::Prompt) },
        { image,
          entry(QStringLiteral("Generate Image"), QSizeF(280, 200),
                { { QStringLiteral("image"), PortType::Image, true, 0.3 },
                  { QStringLiteral("prompt"), PortType::Text, true, 0.55 },
                  { QStringLiteral("run"), PortType::Control, true, 0.8 },
                  { QStringLiteral("result"), PortType::Image, false, 0.5 } },
                NodeKind::Generate) },
        { image,
          entry(QStringLiteral("Edit Image"), QSizeF(280, 200),
                { { QStringLiteral("image"), PortType::Image, true, 0.3 },
                  { QStringLiteral("prompt"), PortType::Text, true, 0.55 },
                  { QStringLiteral("run"), PortType::Control, true, 0.8 },
                  { QStringLiteral("result"), PortType::Image, false, 0.5 } },
                NodeKind::Generate, QStringLiteral("local/procedural-edit-v1"),
                QStringLiteral("Procedural Edit (local)")) },
        { image,
          entry(QStringLiteral("Upscale Image"), QSizeF(240, 160),
                { { QStringLiteral("image"), PortType::Image, true, 0.4 },
                  { QStringLiteral("run"), PortType::Control, true, 0.7 },
                  { QStringLiteral("result"), PortType::Image, false, 0.5 } },
                NodeKind::Generate, QStringLiteral("local/procedural-upscale-v1"),
                QStringLiteral("Procedural Upscale (local)")) },
        { image,
          entry(QStringLiteral("Extract Mask"), QSizeF(240, 160),
                { { QStringLiteral("image"), PortType::Image, true, 0.5 },
                  { QStringLiteral("mask"), PortType::Mask, false, 0.5 } }) },
        { media, compositeNodePrototype(NodeKind::Still) },
        { media, compositeNodePrototype(NodeKind::Video) },
        { compositing, compositeNodePrototype(NodeKind::Blend) },
        { compositing, compositeNodePrototype(NodeKind::Mask) },
        { compositing, compositeNodePrototype(NodeKind::Key) },
        { compositing, compositeNodePrototype(NodeKind::Transform) },
        { video,
          entry(QStringLiteral("Generate Video"), QSizeF(300, 210),
                { { QStringLiteral("prompt"), PortType::Text, true, 0.35 },
                  { QStringLiteral("image"), PortType::Image, true, 0.6 },
                  { QStringLiteral("result"), PortType::Video, false, 0.5 } }) },
        { audio,
          entry(QStringLiteral("Generate Voice"), QSizeF(240, 160),
                { { QStringLiteral("text"), PortType::Text, true, 0.5 },
                  { QStringLiteral("voice"), PortType::Audio, false, 0.5 } }) },
        { control,
          entry(QStringLiteral("Cost Gate"), QSizeF(200, 130),
                { { QStringLiteral("run"), PortType::Control, true, 0.5 },
                  { QStringLiteral("pass"), PortType::Control, false, 0.5 } },
                NodeKind::CostGate) },
        { control,
          entry(QStringLiteral("Batch Count"), QSizeF(200, 130),
                { { QStringLiteral("count"), PortType::Number, false, 0.5 } }) },
        { layout, frame },
    };
}

} // namespace

const QVector<CatalogEntry> &nodeCatalog()
{
    static const QVector<CatalogEntry> catalog = buildCatalog();
    return catalog;
}

Node catalogPrototype(const QString &title)
{
    for (const CatalogEntry &entry : nodeCatalog()) {
        if (entry.prototype.title == title)
            return entry.prototype;
    }
    return Node();
}

} // namespace cutpilot::core
