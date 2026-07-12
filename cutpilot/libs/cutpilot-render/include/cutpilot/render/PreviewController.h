#pragma once

#include <QObject>

namespace cutpilot::render {

class NodeLayerItem;
class PreviewItem;

// Binds the preview surface to the node board: each buffer holds a pinned
// node, chosen explicitly and deliberately decoupled from the canvas
// selection, so one node can be tweaked while a downstream result stays on
// screen. On any relevant change the controller rebuilds each pin's
// composite plan and source set and hands them to the preview item; a pin
// whose node left the graph is dropped.
class PreviewController : public QObject {
    Q_OBJECT

public:
    // The preview's two compare buffers.
    enum class Buffer {
        A = 0,
        B = 1
    };

    explicit PreviewController(QObject *parent = nullptr);

    // The board the pins point into. Graph mutations refresh the preview.
    void setLayer(NodeLayerItem *layer);

    // The surface receiving the built plans; may arrive after pins exist.
    void setPreviewItem(PreviewItem *item);

    void pin(Buffer buffer, int nodeId);
    void unpin(Buffer buffer);
    int pinnedNode(Buffer buffer) const;
    bool anyPinned() const;

public slots:
    // Rebuild both buffers from the live graph and push them to the item.
    void refresh();

signals:
    // A pin was set, cleared, or dropped with its node.
    void pinsChanged();

private:
    NodeLayerItem *m_layer = nullptr;
    PreviewItem *m_item = nullptr;
    int m_pins[2] = { -1, -1 };
};

} // namespace cutpilot::render
