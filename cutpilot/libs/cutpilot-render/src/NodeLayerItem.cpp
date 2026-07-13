#include "cutpilot/render/NodeLayerItem.h"
#include "ConnectorGeometryBuilder.h"
#include "NodeCardLayout.h"
#include "NodeContentRasterizer.h"
#include "NodeGeometryBuilder.h"

#include "cutpilot/core/AlignmentGuides.h"
#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/ConnectorPath.h"
#include "cutpilot/core/command/AddConnectedNodeCommand.h"
#include "cutpilot/core/command/AddNodeCommand.h"
#include "cutpilot/core/command/CompositeCommand.h"
#include "cutpilot/core/command/ConnectCommand.h"
#include "cutpilot/core/command/DeleteNodesCommand.h"
#include "cutpilot/core/command/DisconnectCommand.h"
#include "cutpilot/core/command/EditPromptCommand.h"
#include "cutpilot/core/command/MoveNodesCommand.h"
#include "cutpilot/core/command/SetCompositeParamsCommand.h"
#include "cutpilot/core/command/SetGateLimitCommand.h"
#include "cutpilot/core/command/SetMediaPathCommand.h"
#include "cutpilot/core/command/SetModelCommand.h"

#include <QDir>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <memory>

namespace cutpilot::render {

namespace {

// The marquee outline in the item's logical pixels: a constant hairline at any zoom.
constexpr qreal kMarqueeOutlineWidth = 1.0;

// The alignment-guide hairline width and the pixel radius within which a dragged edge
// snaps a guide into view, both in the item's logical pixels.
constexpr qreal kGuideWidth = 1.0;
constexpr qreal kGuidePixelThreshold = 6.0;

// Connector weight in world units, and the on-screen floor under which the weight
// steps up so a zoomed-out board keeps visible wiring.
constexpr qreal kConnectorWorldWidth = 2.5;
constexpr qreal kConnectorMinScreenWidth = 1.2;

// Port grabbing: the world slack around a port at 100% zoom, and the on-screen
// radius the hit target never shrinks below.
constexpr qreal kPortWorldHitSlack = 3.0;
constexpr qreal kPortMinScreenHitRadius = 5.5;

// The halo drawn around a compatible target port while wiring hovers it.
constexpr qreal kPortHighlightWorldWidth = 3.5;

// The connector pulse accompanying an in-flight generation: frame period of
// one shimmer cycle at the timer's tick rate.
constexpr int kPulseIntervalMs = 33;
constexpr int kPulsePeriodFrames = 36;

// Copy a built mesh into a geometry node, reusing the existing buffers unless the
// vertex or index counts changed.
void uploadMesh(QSGGeometryNode *node, const NodeGeometryBuilder::Mesh &mesh)
{
    QSGGeometry *geometry = node->geometry();
    if (!geometry || geometry->vertexCount() != mesh.vertices.size()
        || geometry->indexCount() != mesh.indices.size()) {
        geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                   mesh.vertices.size(), mesh.indices.size());
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
    }

    auto *vd = geometry->vertexDataAsColoredPoint2D();
    for (int i = 0; i < mesh.vertices.size(); ++i) {
        const auto &v = mesh.vertices[i];
        vd[i].set(v.x, v.y, v.r, v.g, v.b, v.a);
    }
    quint16 *id = geometry->indexDataAsUShort();
    for (int i = 0; i < mesh.indices.size(); ++i)
        id[i] = mesh.indices[i];

    node->markDirty(QSGNode::DirtyGeometry);
}

QSGGeometryNode *makeGeometryNode()
{
    auto *node = new QSGGeometryNode;
    auto *material = new QSGVertexColorMaterial;
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial, true);
    node->setFlag(QSGNode::OwnsGeometry, true);
    return node;
}

// One node card's scene-graph slice: the vertex-colored mesh, an optional
// media texture (the generated result, aspect-fit in the body), and an
// optional content texture (title, prompt, status text). The group remembers
// which node and revisions its textures were built from, so reconciliation
// re-uploads only what actually changed.
class NodeCardGroup : public QSGNode {
public:
    QSGGeometryNode *mesh = nullptr;
    QSGSimpleTextureNode *media = nullptr;
    QSGSimpleTextureNode *content = nullptr;
    int nodeId = -1;
    int contentRevision = -1;
    int mediaVersion = -1;
    bool hadMedia = false;
};

// The generated image sized to fit the node's media well without distortion.
QRectF aspectFitRect(const QRectF &well, const QSizeF &imageSize)
{
    if (imageSize.isEmpty() || well.isEmpty())
        return well;
    const qreal scale = qMin(well.width() / imageSize.width(),
                             well.height() / imageSize.height());
    const QSizeF fitted = imageSize * scale;
    return QRectF(well.center() - QPointF(fitted.width() / 2.0, fitted.height() / 2.0),
                  fitted);
}

} // namespace

NodeLayerItem::NodeLayerItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setFocus(true);

    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(kPulseIntervalMs);
    connect(m_pulseTimer, &QTimer::timeout, this, [this] {
        ++m_pulseFrame;
        update();
    });
}

void NodeLayerItem::setController(CanvasController *controller)
{
    if (m_controller == controller)
        return;

    if (m_controller)
        m_controller->disconnect(this);

    m_controller = controller;

    if (m_controller) {
        // Repaint the node layer whenever the shared camera changes so nodes stay
        // pinned to the world as the canvas pans and zooms.
        connect(m_controller, &CanvasController::cameraChanged, this,
                [this] { update(); });
    }

    emit controllerChanged();
    update();
}

core::Node NodeLayerItem::defaultNode(const QPointF &worldCentre) const
{
    core::Node node;
    node.kind = core::NodeKind::Generate;
    node.title = QStringLiteral("Generate Image");
    const QSizeF size(280.0, 200.0);
    node.worldSize = size;
    node.worldPos =
        worldCentre - QPointF(size.width() / 2.0, size.height() / 2.0);
    node.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
        { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
        { QStringLiteral("run"), core::PortType::Control, true, 0.8 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    return node;
}

void NodeLayerItem::seedStarterNode()
{
    // The starter board is already a pipeline: a prompt feeding a generation
    // whose result feeds an upscale.
    core::Node prompt;
    prompt.kind = core::NodeKind::Prompt;
    prompt.title = QStringLiteral("Prompt");
    prompt.promptText =
        QStringLiteral("A lighthouse on a stormy cliff at dusk, cinematic");
    prompt.worldPos = QPointF(140.0, 160.0);
    prompt.worldSize = QSizeF(260.0, 170.0);
    prompt.ports = {
        { QStringLiteral("text"), core::PortType::Text, false, 0.5 },
    };
    const int promptId = m_graph.addNode(prompt);

    const int generateId = m_graph.addNode(defaultNode(QPointF(720.0, 250.0)));

    core::Node upscale;
    upscale.kind = core::NodeKind::Generate;
    upscale.title = QStringLiteral("Upscale Image");
    upscale.modelId = QStringLiteral("local/procedural-upscale-v1");
    upscale.modelLabel = QStringLiteral("Procedural Upscale (local)");
    upscale.worldSize = QSizeF(240.0, 160.0);
    upscale.worldPos = QPointF(960.0, 190.0);
    upscale.ports = {
        { QStringLiteral("image"), core::PortType::Image, true, 0.4 },
        { QStringLiteral("run"), core::PortType::Control, true, 0.7 },
        { QStringLiteral("result"), core::PortType::Image, false, 0.5 },
    };
    const int upscaleId = m_graph.addNode(upscale);

    core::Connection promptWire;
    promptWire.fromNodeId = promptId;
    promptWire.fromPortIndex = 0;
    promptWire.toNodeId = generateId;
    promptWire.toPortIndex = 1;
    m_graph.addConnection(promptWire);

    core::Connection resultWire;
    resultWire.fromNodeId = generateId;
    resultWire.fromPortIndex = 3;
    resultWire.toNodeId = upscaleId;
    resultWire.toPortIndex = 0;
    m_graph.addConnection(resultWire);

    syncSpatialIndex();
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::seedCompositeBoard()
{
    // Two generated stills: a dusk-gradient backdrop, and a subject on a
    // solid green field for the key to remove.
    QImage backdrop(768, 512, QImage::Format_RGBA8888);
    {
        QPainter painter(&backdrop);
        QLinearGradient sky(0, 0, 0, backdrop.height());
        sky.setColorAt(0.0, QColor(24, 32, 68));
        sky.setColorAt(0.6, QColor(120, 60, 96));
        sky.setColorAt(1.0, QColor(226, 128, 60));
        painter.fillRect(backdrop.rect(), sky);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 214, 140, 200));
        painter.drawEllipse(QPointF(560, 150), 46, 46);
    }

    const QColor field(0, 177, 64);
    QImage subject(768, 512, QImage::Format_RGBA8888);
    {
        QPainter painter(&subject);
        painter.fillRect(subject.rect(), field);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(236, 240, 244));
        painter.drawEllipse(QPointF(384, 200), 90, 90);
        painter.setBrush(QColor(52, 120, 190));
        painter.drawRoundedRect(QRectF(294, 300, 180, 170), 24, 24);
    }

    // A per-process directory: fixed filenames in the shared temp root
    // would collide between instances and hand another local process a
    // predictable path to pre-create.
    static QTemporaryDir boardDir;
    const QDir dir(boardDir.isValid()
                       ? boardDir.path()
                       : QStandardPaths::writableLocation(
                             QStandardPaths::TempLocation));
    const QString backdropPath =
        dir.filePath(QStringLiteral("cutpilot-composite-backdrop.png"));
    const QString subjectPath =
        dir.filePath(QStringLiteral("cutpilot-composite-subject.png"));
    backdrop.save(backdropPath);
    subject.save(subjectPath);

    core::Node backdropStill =
        core::compositeNodePrototype(core::NodeKind::Still);
    backdropStill.title = QStringLiteral("Backdrop");
    backdropStill.mediaPath = backdropPath;
    backdropStill.worldPos = QPointF(120.0, 120.0);
    const int backdropId = m_graph.addNode(backdropStill);

    core::Node subjectStill = core::compositeNodePrototype(core::NodeKind::Still);
    subjectStill.title = QStringLiteral("Subject");
    subjectStill.mediaPath = subjectPath;
    subjectStill.worldPos = QPointF(120.0, 420.0);
    const int subjectId = m_graph.addNode(subjectStill);

    core::Node key = core::compositeNodePrototype(core::NodeKind::Key);
    key.comp.keyColor = field;
    key.comp.keyTolerance = 0.22;
    key.comp.keySoftness = 0.06;
    key.worldPos = QPointF(500.0, 420.0);
    const int keyId = m_graph.addNode(key);

    core::Node transform = core::compositeNodePrototype(core::NodeKind::Transform);
    transform.comp.scale = 0.85;
    transform.comp.translateX = -0.16;
    transform.comp.translateY = 0.05;
    transform.worldPos = QPointF(880.0, 420.0);
    const int transformId = m_graph.addNode(transform);

    core::Node blend = core::compositeNodePrototype(core::NodeKind::Blend);
    blend.worldPos = QPointF(1260.0, 640.0);
    const int blendId = m_graph.addNode(blend);

    const auto connect = [this](int from, int fromPort, int to, int toPort) {
        core::Connection wire;
        wire.fromNodeId = from;
        wire.fromPortIndex = fromPort;
        wire.toNodeId = to;
        wire.toPortIndex = toPort;
        m_graph.addConnection(wire);
    };
    connect(subjectId, 0, keyId, 0);
    connect(keyId, 1, transformId, 0);
    connect(backdropId, 0, blendId, 0);
    connect(transformId, 1, blendId, 1);

    setNodeMedia(backdropId, backdrop);
    setNodeMedia(subjectId, subject);

    syncSpatialIndex();
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::seedStressBoard(int count)
{
    constexpr int kMaxStressNodes = 5000;
    const int total = qBound(0, count, kMaxStressNodes);
    if (total == 0)
        return;

    // A wide grid: spacing well beyond a node's size so any viewport holds only a
    // fraction of the board.
    const int columns = qMax(1, qCeil(qSqrt(qreal(total))));
    constexpr qreal spacingX = 520.0;
    constexpr qreal spacingY = 420.0;
    const QPointF centreOffset(140.0, 100.0);

    int previousId = -1;
    for (int i = 0; i < total; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        const QPointF topLeft(column * spacingX, row * spacingY);
        core::Node node = defaultNode(topLeft + centreOffset);
        node.title = QStringLiteral("Node %1").arg(i + 1);
        const int id = m_graph.addNode(node);

        // Chain the board output-to-input so the stress load carries as many
        // connectors as nodes; each row wrap yields a backward-target curve.
        if (previousId != -1) {
            core::Connection wire;
            wire.fromNodeId = previousId;
            wire.fromPortIndex = 3;
            wire.toNodeId = id;
            wire.toPortIndex = 0;
            m_graph.addConnection(wire);
        }
        previousId = id;
    }

    syncSpatialIndex();
    m_geometryDirty = true;
    update();
}

core::ComfyImportOutcome
NodeLayerItem::importComfyWorkflow(const QJsonObject &result,
                                   const QPointF &worldOrigin)
{
    const core::ComfyImportOutcome outcome =
        core::applyComfyImport(m_graph, m_commands, result, worldOrigin);
    if (outcome.ok) {
        syncSpatialIndex();
        m_geometryDirty = true;
        update();
        emit graphMutated();
    }
    return outcome;
}

void NodeLayerItem::addNodeAtCursor(const QPointF &worldPoint)
{
    m_commands.push(std::make_unique<core::AddNodeCommand>(defaultNode(worldPoint)),
                    m_graph);
    syncSpatialIndex();
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::setNodePrompt(int nodeId, const QString &text)
{
    const core::Node *node = m_graph.nodeById(nodeId);
    if (!node || node->promptText == text)
        return;
    m_commands.push(std::make_unique<core::EditPromptCommand>(nodeId, text), m_graph);
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::setNodeModel(int nodeId, const QString &modelId,
                                 const QString &modelLabel)
{
    const core::Node *node = m_graph.nodeById(nodeId);
    if (!node || (node->modelId == modelId && node->modelLabel == modelLabel))
        return;
    m_commands.push(std::make_unique<core::SetModelCommand>(nodeId, modelId, modelLabel),
                    m_graph);
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::setGateLimit(int nodeId, double limitUsd)
{
    const core::Node *node = m_graph.nodeById(nodeId);
    if (!node || node->kind != core::NodeKind::CostGate
        || node->gateLimitUsd == limitUsd || limitUsd < 0.0)
        return;
    m_commands.push(std::make_unique<core::SetGateLimitCommand>(nodeId, limitUsd),
                    m_graph);
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::setNodeMediaPath(int nodeId, const QString &mediaPath)
{
    const core::Node *node = m_graph.nodeById(nodeId);
    if (!node
        || (node->kind != core::NodeKind::Still
            && node->kind != core::NodeKind::Video)
        || node->mediaPath == mediaPath)
        return;
    m_commands.push(std::make_unique<core::SetMediaPathCommand>(nodeId, mediaPath),
                    m_graph);
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::previewCompositeParams(int nodeId,
                                           const core::CompositeParams &params)
{
    core::Node *node = m_graph.nodeById(nodeId);
    if (!node || !core::isCompositeKind(node->kind) || node->comp == params)
        return;
    node->comp = params;
    node->bumpContent();
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::commitCompositeParams(int nodeId,
                                          const core::CompositeParams &before,
                                          const core::CompositeParams &after)
{
    const core::Node *node = m_graph.nodeById(nodeId);
    if (!node || !core::isCompositeKind(node->kind) || before == after)
        return;
    // The values are already live from the scrub; record the gesture as one
    // undo step rather than applying it a second time.
    m_commands.record(
        std::make_unique<core::SetCompositeParamsCommand>(nodeId, before, after));
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::setNodeMedia(int nodeId, const QImage &image)
{
    if (image.isNull())
        return;
    m_mediaImages.insert(nodeId, image);
    m_mediaVersions[nodeId] = m_mediaVersions.value(nodeId, 0) + 1;
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::clearNodeMedia(int nodeId)
{
    if (!m_mediaImages.contains(nodeId))
        return;
    m_mediaImages.remove(nodeId);
    m_mediaVersions[nodeId] = m_mediaVersions.value(nodeId, 0) + 1;
    m_geometryDirty = true;
    update();
}

void NodeLayerItem::refreshNode(int nodeId)
{
    if (!m_graph.nodeById(nodeId))
        return;
    m_geometryDirty = true;
    updatePulseTimer();
    update();
}

QRectF NodeLayerItem::nodeBodyScreenRect(int nodeId) const
{
    const core::Node *node = m_graph.nodeById(nodeId);
    if (!node)
        return QRectF();
    const QRectF body = NodeCardLayout::bodyRect(*node);
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    return QRectF(localFromWorld(body.topLeft()), body.size() * zoom);
}

bool NodeLayerItem::generationPulseActive() const
{
    return m_pulseTimer->isActive();
}

void NodeLayerItem::updatePulseTimer()
{
    bool anyInFlight = false;
    for (const core::Node &node : m_graph.nodes()) {
        if (node.runState == core::RunState::Queued
            || node.runState == core::RunState::Running) {
            anyInFlight = true;
            break;
        }
    }
    if (anyInFlight && !m_pulseTimer->isActive())
        m_pulseTimer->start();
    else if (!anyInFlight && m_pulseTimer->isActive())
        m_pulseTimer->stop();
}

namespace {

// The stand-in node library the palette offers until the real taxonomy lands. Each
// prototype carries real typed ports, so the type filter and the auto-connect are
// exact even while the list itself is a stub. The generation entries are live:
// edit and upscale carry their local model so they run without a pick.
QVector<core::Node> paletteCatalog()
{
    auto entry = [](const QString &title, const QSizeF &size,
                    const QVector<core::Port> &ports,
                    core::NodeKind kind = core::NodeKind::Blank,
                    const QString &modelId = QString(),
                    const QString &modelLabel = QString()) {
        core::Node node;
        node.kind = kind;
        node.title = title;
        node.worldSize = size;
        node.ports = ports;
        node.modelId = modelId;
        node.modelLabel = modelLabel;
        return node;
    };

    return {
        entry(QStringLiteral("Prompt"), QSizeF(260, 170),
              { { QStringLiteral("text"), core::PortType::Text, false, 0.5 } },
              core::NodeKind::Prompt),
        entry(QStringLiteral("Generate Image"), QSizeF(280, 200),
              { { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
                { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
                { QStringLiteral("run"), core::PortType::Control, true, 0.8 },
                { QStringLiteral("result"), core::PortType::Image, false, 0.5 } },
              core::NodeKind::Generate),
        entry(QStringLiteral("Edit Image"), QSizeF(280, 200),
              { { QStringLiteral("image"), core::PortType::Image, true, 0.3 },
                { QStringLiteral("prompt"), core::PortType::Text, true, 0.55 },
                { QStringLiteral("run"), core::PortType::Control, true, 0.8 },
                { QStringLiteral("result"), core::PortType::Image, false, 0.5 } },
              core::NodeKind::Generate,
              QStringLiteral("local/procedural-edit-v1"),
              QStringLiteral("Procedural Edit (local)")),
        entry(QStringLiteral("Upscale Image"), QSizeF(240, 160),
              { { QStringLiteral("image"), core::PortType::Image, true, 0.4 },
                { QStringLiteral("run"), core::PortType::Control, true, 0.7 },
                { QStringLiteral("result"), core::PortType::Image, false, 0.5 } },
              core::NodeKind::Generate,
              QStringLiteral("local/procedural-upscale-v1"),
              QStringLiteral("Procedural Upscale (local)")),
        entry(QStringLiteral("Cost Gate"), QSizeF(200, 130),
              { { QStringLiteral("run"), core::PortType::Control, true, 0.5 },
                { QStringLiteral("pass"), core::PortType::Control, false, 0.5 } },
              core::NodeKind::CostGate),
        entry(QStringLiteral("Extract Mask"), QSizeF(240, 160),
              { { QStringLiteral("image"), core::PortType::Image, true, 0.5 },
                { QStringLiteral("mask"), core::PortType::Mask, false, 0.5 } }),
        core::compositeNodePrototype(core::NodeKind::Still),
        core::compositeNodePrototype(core::NodeKind::Video),
        core::compositeNodePrototype(core::NodeKind::Blend),
        core::compositeNodePrototype(core::NodeKind::Mask),
        core::compositeNodePrototype(core::NodeKind::Key),
        core::compositeNodePrototype(core::NodeKind::Transform),
        entry(QStringLiteral("Generate Video"), QSizeF(300, 210),
              { { QStringLiteral("prompt"), core::PortType::Text, true, 0.35 },
                { QStringLiteral("image"), core::PortType::Image, true, 0.6 },
                { QStringLiteral("result"), core::PortType::Video, false, 0.5 } }),
        entry(QStringLiteral("Generate Voice"), QSizeF(240, 160),
              { { QStringLiteral("text"), core::PortType::Text, true, 0.5 },
                { QStringLiteral("voice"), core::PortType::Audio, false, 0.5 } }),
        entry(QStringLiteral("Batch Count"), QSizeF(200, 130),
              { { QStringLiteral("count"), core::PortType::Number, false, 0.5 } }),
    };
}

} // namespace

QVector<NodeLayerItem::PaletteOffer>
NodeLayerItem::paletteOffersFor(core::PortType type, bool anchorIsOutput) const
{
    QVector<PaletteOffer> offers;
    for (const core::Node &candidate : paletteCatalog()) {
        int bestPort = -1;
        core::PortMatch bestMatch = core::PortMatch::Incompatible;
        for (int i = 0; i < candidate.ports.size(); ++i) {
            const core::Port &port = candidate.ports[i];
            if (port.isInput != anchorIsOutput)
                continue; // the new node supplies the opposite end of the wire
            const core::PortMatch match = anchorIsOutput
                ? core::portMatch(type, port.type)
                : core::portMatch(port.type, type);
            if (match == core::PortMatch::Incompatible)
                continue;
            if (bestPort == -1
                || (bestMatch == core::PortMatch::Converted
                    && match == core::PortMatch::Direct)) {
                bestPort = i;
                bestMatch = match;
            }
        }
        if (bestPort != -1)
            offers.push_back({ candidate, bestPort });
    }
    return offers;
}

QStringList NodeLayerItem::paletteEntryTitles() const
{
    QStringList titles;
    titles.reserve(m_paletteOffers.size());
    for (const PaletteOffer &offer : m_paletteOffers)
        titles.push_back(offer.node.title);
    return titles;
}

void NodeLayerItem::placePaletteEntry(int index)
{
    if (!m_palettePending || index < 0 || index >= m_paletteOffers.size()
        || !m_graph.nodeById(m_paletteAnchorNodeId)) {
        cancelPalette();
        return;
    }

    // Place the node so its connecting port lands where the connector was dropped.
    PaletteOffer offer = m_paletteOffers[index];
    core::Node node = offer.node;
    node.worldPos = QPointF(0, 0);
    node.worldPos = m_paletteWorldPos - node.portWorldPosition(offer.portIndex);

    m_commands.push(std::make_unique<core::AddConnectedNodeCommand>(
                        node, m_paletteAnchorNodeId, m_paletteAnchorPortIndex,
                        m_paletteAnchorIsOutput, offer.portIndex),
                    m_graph);
    cancelPalette();
    syncSpatialIndex();
    m_geometryDirty = true;
    update();
    emit graphMutated();
}

void NodeLayerItem::cancelPalette()
{
    m_palettePending = false;
    m_paletteOffers.clear();
    m_paletteAnchorNodeId = -1;
    m_paletteAnchorPortIndex = -1;
    m_paletteAnchorIsOutput = false;
}

void NodeLayerItem::syncSpatialIndex()
{
    m_index.rebuild(m_graph.nodes());
}

int NodeLayerItem::pickTopMost(const QPointF &world) const
{
    // The point query narrows candidates through the index; the top-most is the one
    // latest in the model's z-order (list order).
    const QVector<int> candidates = m_index.queryPoint(world);
    int bestId = -1;
    int bestIndex = -1;
    for (int id : candidates) {
        const int index = m_graph.indexOfId(id);
        if (index > bestIndex) {
            bestIndex = index;
            bestId = id;
        }
    }
    return bestId;
}

NodeLayerItem::PortHit NodeLayerItem::pickPort(const QPointF &world) const
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const qreal reach =
        qMax(NodeGeometryBuilder::kPortRadiusWorld + kPortWorldHitSlack,
             kPortMinScreenHitRadius / (zoom > 0.0 ? zoom : 1.0));

    // Ports sit on node edges, so any point within reach of a port is within reach
    // of its node's bounds: probing the index with the inflated point finds every
    // candidate node. The top-most node's port wins where cards overlap.
    const QRectF probe(world - QPointF(reach, reach), QSizeF(reach * 2.0, reach * 2.0));
    PortHit best;
    int bestZ = -1;
    for (int id : m_index.queryRect(probe)) {
        const int z = m_graph.indexOfId(id);
        if (z <= bestZ)
            continue;
        const core::Node *node = m_graph.nodeById(id);
        if (!node)
            continue;
        const int port = node->portIndexAtWorld(world, reach);
        if (port != -1) {
            best.nodeId = id;
            best.portIndex = port;
            bestZ = z;
        }
    }
    return best;
}

void NodeLayerItem::beginConnectDrag(const PortHit &hit, const QPointF &world)
{
    const core::Node *node = m_graph.nodeById(hit.nodeId);
    if (!node)
        return;
    const core::Port &port = node->ports[hit.portIndex];

    m_connectActive = true;
    m_detachedConnectionId = -1;
    m_targetNodeId = -1;
    m_targetPortIndex = -1;
    m_targetMatch = core::PortMatch::Incompatible;
    m_connectCursorWorld = world;

    if (port.isInput) {
        const int existing = m_graph.connectionAtInput(hit.nodeId, hit.portIndex);
        const core::Connection *edge =
            existing != -1 ? m_graph.connectionById(existing) : nullptr;
        const core::Node *source = edge ? m_graph.nodeById(edge->fromNodeId) : nullptr;
        if (edge && source) {
            // Grabbing an occupied input lifts the edge off it: the far output stays
            // anchored and the freed end follows the cursor to re-route or disconnect.
            m_detachedConnectionId = existing;
            m_anchorNodeId = edge->fromNodeId;
            m_anchorPortIndex = edge->fromPortIndex;
            m_anchorIsOutput = true;
            m_anchorType = source->ports[edge->fromPortIndex].type;
        } else {
            m_anchorNodeId = hit.nodeId;
            m_anchorPortIndex = hit.portIndex;
            m_anchorIsOutput = false;
            m_anchorType = port.type;
        }
    } else {
        m_anchorNodeId = hit.nodeId;
        m_anchorPortIndex = hit.portIndex;
        m_anchorIsOutput = true;
        m_anchorType = port.type;
    }

    setCursor(Qt::CrossCursor);
    m_liveDirty = true;
    update();
}

void NodeLayerItem::updateConnectTarget(const QPointF &world)
{
    m_connectCursorWorld = world;

    int targetNode = -1;
    int targetPort = -1;
    core::PortMatch match = core::PortMatch::Incompatible;

    const PortHit hit = pickPort(world);
    if (hit.nodeId != -1 && hit.nodeId != m_anchorNodeId) {
        const core::Node *node = m_graph.nodeById(hit.nodeId);
        const core::Port &port = node->ports[hit.portIndex];
        targetNode = hit.nodeId;
        targetPort = hit.portIndex;
        // The far end must be the opposite side of the anchor; a same-side port is
        // an incompatible target, cueing no-drop rather than silently ignoring it.
        if (port.isInput == m_anchorIsOutput) {
            match = m_anchorIsOutput ? core::portMatch(m_anchorType, port.type)
                                     : core::portMatch(port.type, m_anchorType);
        }
    }

    m_targetNodeId = targetNode;
    m_targetPortIndex = targetPort;
    m_targetMatch = match;

    const bool refused =
        m_targetNodeId != -1 && m_targetMatch == core::PortMatch::Incompatible;
    setCursor(refused ? Qt::ForbiddenCursor : Qt::CrossCursor);

    m_liveDirty = true;
    update();
}

void NodeLayerItem::finishConnectDrag(const QPointF &world)
{
    // The drop target keys off the actual release position: a fast flick can
    // release without a trailing move there, so the hover pick may be stale.
    updateConnectTarget(world);

    // The anchor and a grabbed edge are re-validated against the live graph:
    // both must have survived the drag for the drop to reference them.
    const core::Node *anchor = m_graph.nodeById(m_anchorNodeId);
    if (!anchor || m_anchorPortIndex < 0
        || m_anchorPortIndex >= anchor->ports.size()) {
        endConnectDrag();
        return;
    }
    const bool detachedAlive = m_detachedConnectionId != -1
        && m_graph.connectionById(m_detachedConnectionId) != nullptr;

    const bool hasTarget = m_targetNodeId != -1
        && m_graph.nodeById(m_targetNodeId) != nullptr
        && m_targetMatch != core::PortMatch::Incompatible;

    if (hasTarget) {
        core::Connection wire;
        if (m_anchorIsOutput) {
            wire.fromNodeId = m_anchorNodeId;
            wire.fromPortIndex = m_anchorPortIndex;
            wire.toNodeId = m_targetNodeId;
            wire.toPortIndex = m_targetPortIndex;
        } else {
            wire.fromNodeId = m_targetNodeId;
            wire.fromPortIndex = m_targetPortIndex;
            wire.toNodeId = m_anchorNodeId;
            wire.toPortIndex = m_anchorPortIndex;
        }

        const int occupantId = m_graph.connectionAtInput(wire.toNodeId, wire.toPortIndex);
        const core::Connection *occupant =
            occupantId != -1 ? m_graph.connectionById(occupantId) : nullptr;
        const bool alreadyWired = occupant && occupant->fromNodeId == wire.fromNodeId
            && occupant->fromPortIndex == wire.fromPortIndex;

        if (alreadyWired) {
            // Dropping back where the edge already runs changes nothing — including
            // a grabbed edge released on its own input.
        } else if (detachedAlive) {
            // Re-route as one undo step: the old edge out, the new edge in.
            auto composite = std::make_unique<core::CompositeCommand>();
            composite->add(
                std::make_unique<core::DisconnectCommand>(m_detachedConnectionId));
            composite->add(std::make_unique<core::ConnectCommand>(wire));
            m_commands.push(std::move(composite), m_graph);
            emit graphMutated();
        } else {
            m_commands.push(std::make_unique<core::ConnectCommand>(wire), m_graph);
            emit graphMutated();
        }
        m_geometryDirty = true;
        endConnectDrag();
        return;
    }

    if (m_targetNodeId != -1 || pickTopMost(world) != -1) {
        // An incompatible port refuses the drop; a node body has nothing to wire. A
        // grabbed edge snaps home untouched in both cases.
        endConnectDrag();
        return;
    }

    if (detachedAlive) {
        // A grabbed edge released on empty canvas disconnects.
        m_commands.push(std::make_unique<core::DisconnectCommand>(m_detachedConnectionId),
                        m_graph);
        m_geometryDirty = true;
        endConnectDrag();
        emit graphMutated();
        return;
    }

    // A fresh connector dropped on empty canvas: offer the type-filtered palette.
    m_palettePending = true;
    m_paletteWorldPos = world;
    m_paletteAnchorNodeId = m_anchorNodeId;
    m_paletteAnchorPortIndex = m_anchorPortIndex;
    m_paletteAnchorIsOutput = m_anchorIsOutput;
    m_paletteOffers = paletteOffersFor(m_anchorType, m_anchorIsOutput);
    endConnectDrag();
    emit paletteRequested();
}

void NodeLayerItem::endConnectDrag()
{
    m_connectActive = false;
    m_detachedConnectionId = -1;
    m_targetNodeId = -1;
    m_targetPortIndex = -1;
    m_targetMatch = core::PortMatch::Incompatible;
    m_liveDirty = true;
    unsetCursor();
    update();
}

QVector<int> NodeLayerItem::visibleForViewport() const
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const QSizeF viewport(width(), height());
    return core::visibleIds(m_index, zoom, panLogical(), viewport);
}

qreal NodeLayerItem::devicePixelRatio() const
{
    return window() ? window()->effectiveDevicePixelRatio() : 1.0;
}

QPointF NodeLayerItem::panLogical() const
{
    if (!m_controller)
        return QPointF(0, 0);
    return m_controller->panPixels() / devicePixelRatio();
}

QPointF NodeLayerItem::worldFromLocal(const QPointF &localLogical) const
{
    if (!m_controller)
        return localLogical;
    // The item's local coordinates are logical pixels; the camera works in physical
    // pixels, so scale up by the device pixel ratio before unprojecting.
    const qreal dpr = devicePixelRatio();
    return m_controller->worldFromScreen(localLogical * dpr, dpr);
}

QPointF NodeLayerItem::localFromWorld(const QPointF &world) const
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    return world * zoom + panLogical();
}

void NodeLayerItem::updateDragGuides()
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;

    // The moving reference is the grabbed node for a single drag, or the selection's
    // bounding box for a multi-node drag.
    const QSet<int> moving(m_dragIds.cbegin(), m_dragIds.cend());
    QRectF movingRect;
    for (int id : m_dragIds) {
        if (const core::Node *n = m_graph.nodeById(id))
            movingRect = movingRect.isNull() ? n->worldRect()
                                             : movingRect.united(n->worldRect());
    }

    // Guides key only off nodes the user can see: the culled visible set minus the
    // moving nodes. Snapping to off-screen geometry would draw a guide "for no reason",
    // and scoping to the viewport also bounds the per-move cost.
    QVector<QRectF> neighbours;
    for (int id : visibleForViewport()) {
        if (moving.contains(id))
            continue;
        if (const core::Node *n = m_graph.nodeById(id))
            neighbours.push_back(n->worldRect());
    }

    m_guideLinesLogical.clear();
    if (!movingRect.isNull()) {
        const qreal worldThreshold = kGuidePixelThreshold / (zoom != 0.0 ? zoom : 1.0);
        const auto guides = core::computeAlignmentGuides(movingRect, neighbours, worldThreshold);
        for (const core::AlignmentGuide &g : guides) {
            if (g.axis == core::GuideAxis::Vertical) {
                const QPointF a = localFromWorld(QPointF(g.coordinate, g.spanStart));
                const QPointF b = localFromWorld(QPointF(g.coordinate, g.spanEnd));
                m_guideLinesLogical.push_back(QLineF(a, b));
            } else {
                const QPointF a = localFromWorld(QPointF(g.spanStart, g.coordinate));
                const QPointF b = localFromWorld(QPointF(g.spanEnd, g.coordinate));
                m_guideLinesLogical.push_back(QLineF(a, b));
            }
        }
    }

    m_guidesActive = !m_guideLinesLogical.isEmpty();
    m_overlayDirty = true;
}

QSGNode *NodeLayerItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // Runs on the render thread during the scene-graph sync, with the GUI thread blocked.
    // Reading the model and camera here is safe only under that guarantee: all mutation is
    // GUI-thread. A non-blocking sync or any worker-thread edit would race m_graph/m_controller.
    QSGNode *root = oldNode;
    QSGTransformNode *camera = nullptr;
    if (!root) {
        // A plain container: a camera transform child holding the world-space groups
        // back to front — connector chunks, node cards, live wiring — plus, on
        // demand, screen-space overlay children drawn in the item's logical pixels.
        root = new QSGNode;
        camera = new QSGTransformNode;
        camera->appendChildNode(new QSGNode); // connector chunks
        camera->appendChildNode(new QSGNode); // node cards
        camera->appendChildNode(new QSGNode); // live wiring
        root->appendChildNode(camera);
        m_geometryDirty = true;
        m_overlayDirty = true;
        m_liveDirty = true;
        m_lastVisibleConnections.clear();
        m_lastConnectorWidth = 0.0;
    } else {
        camera = static_cast<QSGTransformNode *>(root->firstChild());
    }

    QSGNode *connectorRoot = camera->firstChild();
    QSGNode *nodeRoot = connectorRoot->nextSibling();
    QSGNode *liveRoot = nodeRoot->nextSibling();

    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const QPointF pan = panLogical();

    // The camera transform maps world coordinates to the item's logical pixels, so a
    // pan or zoom is a matrix change here and never re-triangulates any node.
    QMatrix4x4 matrix;
    matrix.translate(float(pan.x()), float(pan.y()));
    matrix.scale(float(zoom), float(zoom));
    camera->setMatrix(matrix);

    // Cull to the nodes overlapping the viewport. The subtree is rebuilt when the model
    // changed, the detail tier flipped, or the visible membership changed; a pan or
    // in-tier zoom whose visible set is unchanged re-sets only the transform matrix.
    const QVector<int> visibleList = visibleForViewport();
    const QSet<int> visible(visibleList.cbegin(), visibleList.cend());
    const bool membershipChanged = visible != m_lastVisibleSet;
    const bool modelDirty = m_geometryDirty;
    m_geometryDirty = false;

    // Connectors are culled on their own curve bounds — a long edge can cross the
    // viewport while both endpoint nodes sit off-screen — and rebuilt when the model
    // changed, their visible membership changed, or the width step flipped.
    const qreal connectorWidth = connectorWorldWidth(zoom);
    const QVector<int> visibleWires = visibleConnections();
    if (modelDirty || connectorWidth != m_lastConnectorWidth
        || visibleWires != m_lastVisibleConnections
        || m_pulseFrame != m_lastPulseFrame) {
        rebuildConnectors(connectorRoot, visibleWires);
        m_lastVisibleConnections = visibleWires;
        m_lastConnectorWidth = connectorWidth;
        m_lastPulseFrame = m_pulseFrame;
    }

    const bool detailed = zoom >= NodeGeometryBuilder::kDetailZoom;
    if (modelDirty || detailed != m_lastDetailed || membershipChanged) {
        rebuildNodes(nodeRoot, detailed, visible);
        m_lastDetailed = detailed;
        m_lastVisibleSet = visible;
    }

    updateLive(liveRoot);
    updateOverlay(root, camera);
    return root;
}

void NodeLayerItem::rebuildNodes(QSGNode *nodeRoot, bool detailed,
                                 const QSet<int> &visible)
{
    NodeGeometryBuilder builder;
    NodeContentRasterizer rasterizer;

    QSGNode *child = nodeRoot->firstChild();
    for (const core::Node &node : m_graph.nodes()) {
        if (!visible.contains(node.id))
            continue; // off-screen nodes are not drawn

        auto *group = static_cast<NodeCardGroup *>(child);
        if (!group) {
            group = new NodeCardGroup;
            group->mesh = makeGeometryNode();
            group->appendChildNode(group->mesh);
            nodeRoot->appendChildNode(group);
        }
        const bool reassigned = group->nodeId != node.id;
        group->nodeId = node.id;

        uploadMesh(group->mesh, builder.buildNode(node, m_theme, detailed));

        // The media texture: the decoded result, aspect-fit into the media
        // well, re-uploaded only when a new result replaces the old.
        const QImage mediaImage =
            detailed ? m_mediaImages.value(node.id) : QImage();
        if (!mediaImage.isNull()) {
            if (!group->media) {
                group->media = new QSGSimpleTextureNode;
                group->media->setOwnsTexture(true);
                group->media->setFiltering(QSGTexture::Linear);
                // Media sits over the mesh and under the text content.
                group->insertChildNodeAfter(group->media, group->mesh);
            }
            const int mediaVersion = m_mediaVersions.value(node.id, 0);
            if (reassigned || group->mediaVersion != mediaVersion) {
                group->media->setTexture(
                    window()->createTextureFromImage(mediaImage));
                group->mediaVersion = mediaVersion;
            }
            group->media->setRect(
                aspectFitRect(NodeCardLayout::bodyRect(node), mediaImage.size()));
        } else if (group->media) {
            group->removeChildNode(group->media);
            delete group->media;
            group->media = nullptr;
            group->mediaVersion = -1;
        }

        // The content texture: the card's text layer, re-rasterized only when
        // the node's content revision moved. The scene-graph sync blocks the
        // GUI thread, so reading the node and painting a QImage here is safe.
        if (detailed) {
            if (!group->content) {
                group->content = new QSGSimpleTextureNode;
                group->content->setOwnsTexture(true);
                group->content->setFiltering(QSGTexture::Linear);
                group->appendChildNode(group->content);
            }
            // Media arriving or leaving redraws the text layer too, so the
            // placeholder guidance never lingers over a thumbnail.
            const bool mediaPresent = !mediaImage.isNull();
            if (reassigned || group->contentRevision != node.contentRevision
                || group->hadMedia != mediaPresent) {
                group->content->setTexture(window()->createTextureFromImage(
                    rasterizer.rasterize(node, m_theme, mediaPresent)));
                group->contentRevision = node.contentRevision;
                group->hadMedia = mediaPresent;
            }
            group->content->setRect(node.worldRect());
        } else if (group->content) {
            group->removeChildNode(group->content);
            delete group->content;
            group->content = nullptr;
            group->contentRevision = -1;
        }

        child = group->nextSibling();
    }

    // Drop any card groups left over from a larger previous board.
    while (child) {
        QSGNode *next = child->nextSibling();
        nodeRoot->removeChildNode(child);
        delete child;
        child = next;
    }
}

qreal NodeLayerItem::connectorWorldWidth(qreal zoom)
{
    qreal width = kConnectorWorldWidth;
    if (zoom <= 0.0)
        return width;
    while (width * zoom < kConnectorMinScreenWidth && width < 512.0)
        width *= 2.0;
    return width;
}

QVector<int> NodeLayerItem::visibleConnections() const
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    const QRectF world =
        core::viewportWorldRect(zoom, panLogical(), QSizeF(width(), height()));
    const qreal pad = connectorWorldWidth(zoom);

    QVector<int> ids;
    for (const core::Connection &c : m_graph.connections()) {
        if (m_connectActive && c.id == m_detachedConnectionId)
            continue; // a grabbed edge is carried by the live overlay instead
        const core::Node *from = m_graph.nodeById(c.fromNodeId);
        const core::Node *to = m_graph.nodeById(c.toNodeId);
        if (!from || !to || c.fromPortIndex >= from->ports.size()
            || c.toPortIndex >= to->ports.size())
            continue;
        const QPointF a = from->portWorldPosition(c.fromPortIndex);
        const QPointF b = to->portWorldPosition(c.toPortIndex);
        if (core::connectorBounds(a, b, pad).intersects(world))
            ids.push_back(c.id);
    }
    return ids;
}

void NodeLayerItem::rebuildConnectors(QSGNode *connectorRoot, const QVector<int> &ids)
{
    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;

    QVector<ConnectorDraw> draws;
    draws.reserve(ids.size());
    for (int id : ids) {
        const core::Connection *c = m_graph.connectionById(id);
        const core::Node *from = m_graph.nodeById(c->fromNodeId);
        const core::Node *to = m_graph.nodeById(c->toNodeId);
        const core::PortType fromType = from->ports[c->fromPortIndex].type;
        const core::PortType toType = to->ports[c->toPortIndex].type;

        ConnectorDraw draw;
        draw.from = from->portWorldPosition(c->fromPortIndex);
        draw.to = to->portWorldPosition(c->toPortIndex);
        // The wire wears the color of what flows: the source output's type. A
        // permitted implicit conversion dashes the tail near the target.
        draw.color = NodeGeometryBuilder::portColor(fromType, m_theme);
        draw.dashedTail =
            core::portMatch(fromType, toType) == core::PortMatch::Converted;

        // Wires into an in-flight generation shimmer toward the running
        // color, making the active path readable at a glance.
        if (to->runState == core::RunState::Queued
            || to->runState == core::RunState::Running) {
            const qreal phase =
                qreal(m_pulseFrame % kPulsePeriodFrames) / kPulsePeriodFrames;
            const qreal wave = 0.5 + 0.5 * qSin(phase * 2.0 * M_PI);
            const QColor target = m_theme.statusRunning();
            const qreal mix = 0.25 + 0.55 * wave;
            draw.color = QColor::fromRgbF(
                draw.color.redF() + (target.redF() - draw.color.redF()) * mix,
                draw.color.greenF() + (target.greenF() - draw.color.greenF()) * mix,
                draw.color.blueF() + (target.blueF() - draw.color.blueF()) * mix);
        }
        draws.push_back(draw);
    }

    ConnectorGeometryBuilder builder;
    const QVector<ConnectorGeometryBuilder::Mesh> chunks =
        builder.buildConnectors(draws, connectorWorldWidth(zoom));

    QSGNode *child = connectorRoot->firstChild();
    for (const ConnectorGeometryBuilder::Mesh &chunk : chunks) {
        auto *geometryNode = static_cast<QSGGeometryNode *>(child);
        if (!geometryNode) {
            geometryNode = makeGeometryNode();
            connectorRoot->appendChildNode(geometryNode);
        }
        uploadMesh(geometryNode, chunk);
        child = geometryNode->nextSibling();
    }
    while (child) {
        QSGNode *next = child->nextSibling();
        connectorRoot->removeChildNode(child);
        delete child;
        child = next;
    }
}

void NodeLayerItem::updateLive(QSGNode *liveRoot)
{
    auto *geometryNode = static_cast<QSGGeometryNode *>(liveRoot->firstChild());

    if (!m_connectActive) {
        if (geometryNode) {
            liveRoot->removeChildNode(geometryNode);
            delete geometryNode;
        }
        m_liveDirty = false;
        return;
    }

    if (!geometryNode) {
        geometryNode = makeGeometryNode();
        liveRoot->appendChildNode(geometryNode);
        m_liveDirty = true;
    }
    if (!m_liveDirty)
        return;

    const core::Node *anchorNode = m_graph.nodeById(m_anchorNodeId);
    if (!anchorNode) {
        uploadMesh(geometryNode, NodeGeometryBuilder::Mesh{});
        m_liveDirty = false;
        return;
    }

    const QPointF anchorPos = anchorNode->portWorldPosition(m_anchorPortIndex);
    const bool hasTarget =
        m_targetNodeId != -1 && m_targetMatch != core::PortMatch::Incompatible;

    // The free end snaps onto a compatible target port, otherwise it rides the cursor.
    QPointF freeEnd = m_connectCursorWorld;
    const core::Node *targetNode =
        m_targetNodeId != -1 ? m_graph.nodeById(m_targetNodeId) : nullptr;
    if (hasTarget && targetNode)
        freeEnd = targetNode->portWorldPosition(m_targetPortIndex);

    ConnectorDraw draw;
    if (m_anchorIsOutput) {
        draw.from = anchorPos;
        draw.to = freeEnd;
    } else {
        draw.from = freeEnd;
        draw.to = anchorPos;
    }
    draw.color = NodeGeometryBuilder::portColor(m_anchorType, m_theme);
    draw.dashedTail = hasTarget && m_targetMatch == core::PortMatch::Converted;

    // Over an incompatible port the wire itself goes gray: the no-drop cue that
    // pairs with the forbidden cursor, so refusal never leans on the cursor alone.
    if (m_targetNodeId != -1 && m_targetMatch == core::PortMatch::Incompatible)
        draw.color = m_theme.borderDefault();

    const qreal zoom = m_controller ? m_controller->zoom() : 1.0;
    ConnectorGeometryBuilder builder;
    ConnectorGeometryBuilder::Mesh mesh =
        builder.buildSingle(draw, connectorWorldWidth(zoom));

    // A neutral halo lifts the port the drop would land on.
    if (hasTarget && targetNode) {
        const core::Port &port = targetNode->ports[m_targetPortIndex];
        const QPointF centre = targetNode->portWorldPosition(m_targetPortIndex);
        const qreal reach =
            NodeGeometryBuilder::kPortRadiusWorld + kPortHighlightWorldWidth;
        if (port.type == core::PortType::Control) {
            NodeGeometryBuilder::appendQuad(mesh,
                                            QRectF(centre - QPointF(reach, reach),
                                                   QSizeF(reach * 2.0, reach * 2.0)),
                                            m_theme.glowEmphasis());
        } else {
            NodeGeometryBuilder::appendDisc(mesh, centre, reach, m_theme.glowEmphasis());
        }
    }

    uploadMesh(geometryNode, mesh);
    m_liveDirty = false;
}

void NodeLayerItem::updateOverlay(QSGNode *root, QSGTransformNode *camera)
{
    // A single screen-space overlay child sits beside the camera transform, so its
    // vertices draw in the item's logical pixels. The marquee band and the alignment
    // guides are mutually exclusive gestures, so one geometry node carries whichever is
    // active.
    auto *overlay = static_cast<QSGGeometryNode *>(camera->nextSibling());
    const bool active = m_marqueeActive || m_guidesActive;

    if (active) {
        if (!overlay) {
            overlay = makeGeometryNode();
            root->appendChildNode(overlay);
            m_overlayDirty = true;
        }
        if (m_overlayDirty) {
            NodeGeometryBuilder builder;
            if (m_marqueeActive) {
                const QRectF rect =
                    QRectF(m_marqueeStartLogical, m_marqueeCurrentLogical).normalized();
                uploadMesh(overlay,
                           builder.buildScreenRect(rect, m_theme.selectionFill(),
                                                   m_theme.selection(),
                                                   kMarqueeOutlineWidth));
            } else {
                uploadMesh(overlay, builder.buildScreenLines(m_guideLinesLogical,
                                                             m_theme.emphasis(),
                                                             kGuideWidth));
            }
        }
    } else if (overlay) {
        root->removeChildNode(overlay);
        delete overlay;
    }

    m_overlayDirty = false;
}

void NodeLayerItem::mousePressEvent(QMouseEvent *event)
{
    if (!m_controller) {
        event->ignore();
        return;
    }
    forceActiveFocus();

    // Middle-button, or Space + left-button, pans the canvas; handled on the top layer
    // so a node hit and a pan never fight over the same press.
    const bool panButton = event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && m_spaceHeld);
    if (panButton) {
        if (!m_panning) {
            m_panning = true;
            m_panButton = event->button();
            m_lastPanPosLogical = event->position();
            setCursor(Qt::ClosedHandCursor);
        }
        event->accept();
        return;
    }

    // A right press on a node raises its menu — run entries on a generation
    // node, preview pinning on anything that produces an image; the chrome
    // assembles the entries by kind. The node becomes the selection so the
    // menu clearly targets it.
    if (event->button() == Qt::RightButton) {
        const int menuId = pickTopMost(worldFromLocal(event->position()));
        if (menuId != -1 && m_graph.nodeById(menuId)) {
            m_graph.selectOnly(menuId);
            m_graph.raiseToTop(menuId);
            m_geometryDirty = true;
            update();
            emit nodeMenuRequested(menuId);
            event->accept();
            return;
        }
        event->ignore();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPointF world = worldFromLocal(event->position());
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);

    // A grab on a port starts wiring, not selection; Shift keeps the click available
    // for toggling the node under the port. Ports are only grabbable while they are
    // drawn — the low-zoom tier hides them and keeps the whole card draggable.
    const qreal zoom = m_controller->zoom();
    if (!shift && zoom >= NodeGeometryBuilder::kDetailZoom) {
        const PortHit portHit = pickPort(world);
        if (portHit.nodeId != -1) {
            beginConnectDrag(portHit, world);
            event->accept();
            return;
        }
    }

    const int hitId = pickTopMost(world);

    if (hitId != -1 && !shift && zoom >= NodeGeometryBuilder::kDetailZoom) {
        const core::Node *hitNode = m_graph.nodeById(hitId);
        if (hitNode->kind == core::NodeKind::Generate) {
            const bool inFlight = hitNode->runState == core::RunState::Queued
                || hitNode->runState == core::RunState::Running;
            if (NodeCardLayout::runButtonRect(*hitNode).contains(world)) {
                m_graph.selectOnly(hitId);
                m_graph.raiseToTop(hitId);
                m_geometryDirty = true;
                update();
                if (inFlight)
                    emit stopRequested(hitId);
                else
                    emit runRequested(hitId);
                event->accept();
                return;
            }
            if (NodeCardLayout::modelChipRect(*hitNode).contains(world)) {
                m_graph.selectOnly(hitId);
                m_graph.raiseToTop(hitId);
                m_geometryDirty = true;
                update();
                emit modelPickerRequested(hitId);
                event->accept();
                return;
            }
        }
    }

    if (hitId != -1) {
        if (shift) {
            m_graph.toggleSelected(hitId);
        } else if (!m_graph.nodeById(hitId)->selected) {
            // A plain click on an unselected node makes it the only selection; a plain
            // click on an already-selected node keeps the whole selection to drag it.
            m_graph.selectOnly(hitId);
        }

        if (m_graph.nodeById(hitId) && m_graph.nodeById(hitId)->selected) {
            const QVector<int> ids = m_graph.selectedIds();
            // Raise-on-touch is a deliberate, non-undoable z reorder: it is not routed
            // through the command stack, so add/move/delete undo never restores a prior z.
            m_graph.raiseToTop(ids);
            m_dragging = true;
            m_dragIds = m_graph.selectedIds();
            m_dragPressWorld = world;
            m_dragLastWorld = world;
            setCursor(Qt::ClosedHandCursor);
        }
        m_geometryDirty = true;
        update();
    } else {
        // Empty canvas: a plain press clears the selection and starts a marquee band;
        // Shift keeps the current selection so the band adds to it.
        if (!shift)
            m_graph.clearSelection();
        m_marqueeActive = true;
        m_marqueeAdditive = shift;
        m_marqueeStartLogical = event->position();
        m_marqueeCurrentLogical = event->position();
        m_overlayDirty = true;
        m_geometryDirty = true;
        update();
    }
    event->accept();
}

void NodeLayerItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning && m_controller) {
        const QPointF deltaLogical = event->position() - m_lastPanPosLogical;
        m_lastPanPosLogical = event->position();
        m_controller->panByPixels(deltaLogical * devicePixelRatio()); // 1:1, no smoothing
        event->accept();
        return;
    }

    if (m_connectActive) {
        updateConnectTarget(worldFromLocal(event->position()));
        event->accept();
        return;
    }

    if (m_marqueeActive) {
        m_marqueeCurrentLogical = event->position();
        m_overlayDirty = true;
        update();
        event->accept();
        return;
    }

    if (m_dragging) {
        const QPointF world = worldFromLocal(event->position());
        const QPointF delta = world - m_dragLastWorld;
        m_dragLastWorld = world;
        m_graph.moveNodesBy(m_dragIds, delta); // live 1:1 feedback
        // Only the dragged nodes moved, so update just their index entries rather than
        // rebuilding the whole index on every move.
        for (int id : m_dragIds) {
            if (const core::Node *n = m_graph.nodeById(id))
                m_index.update(id, n->worldRect());
        }
        updateDragGuides();
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    event->ignore();
}

void NodeLayerItem::mouseReleaseEvent(QMouseEvent *event)
{
    // Each gesture ends only on the release of the button that drives it: the
    // pan on its recorded button, everything else on the left button.
    if (m_panning && event->button() == m_panButton) {
        m_panning = false;
        m_panButton = Qt::NoButton;
        if (m_connectActive)
            setCursor(Qt::CrossCursor);
        else if (!m_spaceHeld)
            unsetCursor();
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    if (m_connectActive) {
        finishConnectDrag(worldFromLocal(event->position()));
        event->accept();
        return;
    }

    if (m_marqueeActive) {
        const QPointF worldStart = worldFromLocal(m_marqueeStartLogical);
        const QPointF worldEnd = worldFromLocal(m_marqueeCurrentLogical);
        m_graph.selectInRect(QRectF(worldStart, worldEnd).normalized(), m_marqueeAdditive);
        m_marqueeActive = false;
        m_overlayDirty = true;
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    if (m_dragging) {
        // Coalesce the whole drag into one net-delta move: the nodes already moved live
        // by this delta, so record (do not re-apply) it as a single undo step.
        const QPointF net = m_dragLastWorld - m_dragPressWorld;
        if (!net.isNull())
            m_commands.record(std::make_unique<core::MoveNodesCommand>(m_dragIds, net));
        m_dragging = false;
        m_dragIds.clear();
        // The guides are a drag hint; clear them when the gesture ends.
        m_guidesActive = false;
        m_guideLinesLogical.clear();
        m_overlayDirty = true;
        unsetCursor();
        update();
        event->accept();
        return;
    }

    event->ignore();
}

void NodeLayerItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_controller || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const QPointF world = worldFromLocal(event->position());
    const int hitId = pickTopMost(world);
    if (hitId == -1) {
        addNodeAtCursor(world); // empty canvas: add a node at the cursor
        event->accept();
        return;
    }

    const core::Node *hitNode = m_graph.nodeById(hitId);
    if (hitNode->kind == core::NodeKind::Prompt) {
        m_graph.selectOnly(hitId);
        m_geometryDirty = true;
        update();
        emit promptEditRequested(hitId);
        event->accept();
        return;
    }
    if (hitNode->kind == core::NodeKind::CostGate) {
        m_graph.selectOnly(hitId);
        m_geometryDirty = true;
        update();
        emit gateLimitEditRequested(hitId);
        event->accept();
        return;
    }
    if (hitNode->kind == core::NodeKind::Still) {
        m_graph.selectOnly(hitId);
        m_geometryDirty = true;
        update();
        emit mediaPickRequested(hitId);
        event->accept();
        return;
    }
    if (hitNode->kind == core::NodeKind::Video) {
        m_graph.selectOnly(hitId);
        m_geometryDirty = true;
        update();
        if (hitNode->mediaPath.isEmpty())
            emit mediaPickRequested(hitId);
        else
            emit videoTransportRequested(hitId);
        event->accept();
        return;
    }
    if (core::isCompositeKind(hitNode->kind)) {
        m_graph.selectOnly(hitId);
        m_geometryDirty = true;
        update();
        emit compositeEditRequested(hitId);
        event->accept();
        return;
    }
    event->ignore(); // on other nodes, a double-click does not add
}

void NodeLayerItem::wheelEvent(QWheelEvent *event)
{
    if (!m_controller) {
        event->ignore();
        return;
    }

    const qreal dpr = devicePixelRatio();
    const QPointF anchorPx = event->position() * dpr;

    // Pinch or Ctrl+wheel zooms toward the cursor; a plain two-finger scroll pans.
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const qreal steps = event->angleDelta().y() / 120.0;
        const qreal factor = qPow(1.15, steps);
        m_controller->zoomAbout(anchorPx, factor, dpr);
        event->accept();
        return;
    }

    QPointF deltaLogical;
    if (!event->pixelDelta().isNull())
        deltaLogical = event->pixelDelta();
    else
        deltaLogical = event->angleDelta() / 4.0;

    m_controller->panByPixels(deltaLogical * dpr);
    event->accept();
}

void NodeLayerItem::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) {
        m_spaceHeld = true;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }

    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);

    if (control && event->key() == Qt::Key_0) {
        if (m_controller)
            m_controller->reset();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        // Cancel an in-flight wire first: the delete may remove its endpoints.
        if (m_connectActive)
            endConnectDrag();
        const QVector<int> ids = m_graph.selectedIds();
        if (!ids.isEmpty()) {
            m_commands.push(std::make_unique<core::DeleteNodesCommand>(ids), m_graph);
            syncSpatialIndex();
            m_geometryDirty = true;
            update();
            emit graphMutated();
        }
        event->accept();
        return;
    }

    if (control && event->key() == Qt::Key_Z) {
        // Cancel an in-flight wire first: the history walk may remove its endpoints.
        if (m_connectActive)
            endConnectDrag();
        if (shift)
            m_commands.redo(m_graph);
        else
            m_commands.undo(m_graph);
        syncSpatialIndex();
        m_geometryDirty = true;
        update();
        emit graphMutated();
        event->accept();
        return;
    }

    if (control && event->key() == Qt::Key_A) {
        for (const core::Node &n : m_graph.nodes())
            m_graph.setSelected(n.id, true);
        m_geometryDirty = true;
        update();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        if (m_connectActive) {
            endConnectDrag(); // abandon the wire; a grabbed edge snaps home
        } else {
            m_graph.clearSelection();
            m_geometryDirty = true;
            update();
        }
        event->accept();
        return;
    }

    event->ignore();
}

void NodeLayerItem::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        if (!m_panning && !m_dragging)
            unsetCursor();
        event->accept();
        return;
    }
    event->ignore();
}

} // namespace cutpilot::render
