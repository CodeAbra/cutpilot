#pragma once

#include <QColor>
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

// What a node's body is made of: a plain content card, editable prompt text
// that feeds downstream generation, a model-backed generation whose body
// becomes the produced media, a cost gate that holds its downstream branch
// once a run's spend would cross the gate's limit, a still image or video
// loaded from a local file, or one of the local compositing operations
// (blend, mask, key, transform) evaluated on the GPU without any vendor
// call. A video feeds its current frame downstream; its transport scrubs at
// proxy resolution. A frame is an organizational backdrop: a tinted, titled
// region drawn behind the nodes resting on it, which move with it.
enum class NodeKind {
    Blank,
    Prompt,
    Generate,
    CostGate,
    Still,
    Video,
    Blend,
    Mask,
    Key,
    Transform,
    Frame
};

// How a blend node combines its over layer with its base. The separable
// standard formulas (as specified for PDF and W3C compositing): Normal is
// plain source-over; the others mix the blended color by the base's alpha
// before compositing.
enum class BlendMode {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Add
};

// Parameters of the local compositing operations. One value struct covers
// every op — each node kind reads only its own fields — so a single undoable
// command and a single inspector seam serve all of them.
struct CompositeParams {
    // Blend: how the over layer combines, and its opacity applied on top of
    // the layer's own alpha.
    BlendMode blendMode = BlendMode::Normal;
    double opacity = 1.0;

    // Mask: multiply the image's alpha by the mask's luminance, optionally
    // inverted so the mask cuts instead of keeps.
    bool invertMask = false;

    // Key: with lumaKey false the key color is removed within tolerance,
    // fading over the softness band; with lumaKey true dark pixels are
    // removed by luminance instead.
    bool lumaKey = false;
    QColor keyColor{ 0, 177, 64 };
    double keyTolerance = 0.30;
    double keySoftness = 0.10;

    // Transform: translation as a fraction of the image's own size, a scale
    // factor, and a rotation about the image center.
    double translateX = 0.0;
    double translateY = 0.0;
    double scale = 1.0;
    double rotationDeg = 0.0;

    bool operator==(const CompositeParams &other) const = default;
};

// A generation node's run lifecycle. NeedsKey marks a run refused because the
// picked model's vendor has no API key configured; the node surfaces the
// add-a-key affordance instead of failing silently. Held marks a node stopped
// by a cost gate or the run cap — nothing is queued in the service, and the
// run can resume it once the limit allows.
enum class RunState {
    Idle,
    Queued,
    Running,
    Done,
    Error,
    NeedsKey,
    Held
};

// The generation service accepts output sides in this range, in pixels. Every
// seam that stores a requested output size holds both sides inside it.
inline constexpr int kOutputSideMin = 64;
inline constexpr int kOutputSideMax = 2048;

// A node on the canvas, expressed entirely in world coordinates. The body is the
// content; the header is a slim strip carrying the title and model name. The
// renderer draws this; the node never becomes a per-instance widget.
struct Node {
    int id = 0;

    // The node's durable identity: a UUID string persisted in the workflow
    // document, unlike the session-scoped integer id. Anything that must
    // recognize a node across save/reload or rename (the quick surface's
    // binding, external references) resolves by uid, never by title.
    // Minted by NodeGraph::addNode when empty; preserved exactly by
    // undo/redo restore and by the document round-trip.
    QString uid;

    QString title;
    QPointF worldPos{0, 0};   // top-left of the card, in world units
    QSizeF worldSize{0, 0};   // card size, in world units
    bool selected = false;
    QVector<Port> ports;

    NodeKind kind = NodeKind::Blank;

    // Prompt-node body text; a Generate node falls back to its own text when
    // no upstream prompt is wired in.
    QString promptText;

    // The picked generation model and its display name for the header chip.
    QString modelId;
    QString modelLabel;

    // The requested output size for a generation, in pixels. Zero means the
    // model's default. A document parameter edited through an undoable
    // command; it keys the result cache, so changing it re-generates. Both
    // sides stay inside [kOutputSideMin, kOutputSideMax] — every seam that
    // writes them enforces the bound, so a stored format can never be
    // refused by the generation service for its size.
    int outputWidth = 0;
    int outputHeight = 0;

    // A cost gate's user-set spend limit for its downstream branch, in USD.
    // A real parameter, edited through an undoable command.
    double gateLimitUsd = 0.05;

    // A still node's source file. A document parameter edited through an
    // undoable command, unlike the transient resultPath a run writes.
    QString mediaPath;

    // A foreign node preserved through workflow import: the external
    // system's type name, and its original payload re-emitted unchanged on
    // export so a round trip loses nothing.
    QString externalType;
    QString externalData;

    // The local compositing parameters. Scrubbing writes them directly for
    // live feedback and records one coalesced command on release, mirroring
    // how a drag moves nodes.
    CompositeParams comp;

    // Live run status, written directly (not through commands): status is
    // transient job state, not an undoable edit.
    RunState runState = RunState::Idle;
    qreal runProgress = 0.0;
    QString statusMessage;

    // The model's price for one run, shown before anything is spent.
    // Transient display state fed from the registry; negative until known.
    double estimatedCostUsd = -1.0;

    // What a gate's branch has spent during the current run. Transient
    // display state; negative when no run has touched the gate.
    double gateSpentUsd = -1.0;

    // The finished result: media path, the result file's SHA-256 (its
    // identity for downstream cache keys), final cost (negative until
    // known), and the produced resolution.
    QString resultPath;
    QString resultDigest;
    double costUsd = -1.0;
    int resultWidth = 0;
    int resultHeight = 0;

    // Bumped on every content or status change so cached rasters and textures
    // know to refresh; geometry-only changes (move, select) leave it alone.
    int contentRevision = 0;

    void bumpContent() { ++contentRevision; }

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
