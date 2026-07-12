#include "cutpilot/render/PreviewController.h"

#include "cutpilot/core/CompositePlan.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/render/PreviewItem.h"

namespace cutpilot::render {

PreviewController::PreviewController(QObject *parent)
    : QObject(parent)
{
}

void PreviewController::setLayer(NodeLayerItem *layer)
{
    if (m_layer == layer)
        return;
    if (m_layer)
        m_layer->disconnect(this);
    m_layer = layer;
    if (m_layer) {
        connect(m_layer, &NodeLayerItem::graphMutated, this,
                &PreviewController::refresh);
    }
    refresh();
}

void PreviewController::setPreviewItem(PreviewItem *item)
{
    if (m_item == item)
        return;
    m_item = item;
    refresh();
}

void PreviewController::pin(Buffer buffer, int nodeId)
{
    const int slot = int(buffer);
    if (m_pins[slot] == nodeId)
        return;
    m_pins[slot] = nodeId;
    refresh();
    emit pinsChanged();
}

void PreviewController::unpin(Buffer buffer)
{
    const int slot = int(buffer);
    if (m_pins[slot] == -1)
        return;
    m_pins[slot] = -1;
    refresh();
    emit pinsChanged();
}

int PreviewController::pinnedNode(Buffer buffer) const
{
    return m_pins[int(buffer)];
}

bool PreviewController::anyPinned() const
{
    return m_pins[0] != -1 || m_pins[1] != -1;
}

void PreviewController::refresh()
{
    if (!m_layer)
        return;

    bool dropped = false;
    for (int slot = 0; slot < 2; ++slot) {
        const int pin = m_pins[slot];
        if (pin == -1) {
            if (m_item)
                m_item->clearBuffer(slot);
            continue;
        }
        if (!m_layer->graph().nodeById(pin)) {
            m_pins[slot] = -1;
            dropped = true;
            if (m_item)
                m_item->clearBuffer(slot);
            continue;
        }
        if (!m_item)
            continue;

        PreviewBufferData data;
        data.plan = core::buildCompositePlan(m_layer->graph(), pin);
        data.active = data.plan.valid;
        for (int sourceId : data.plan.sourceNodeIds) {
            PreviewSource source;
            source.nodeId = sourceId;
            source.image = m_layer->nodeMediaImage(sourceId);
            source.version = m_layer->nodeMediaVersion(sourceId);
            if (!source.image.isNull())
                data.sources.push_back(source);
        }
        m_item->setBuffer(slot, data);
    }

    if (dropped)
        emit pinsChanged();
}

} // namespace cutpilot::render
