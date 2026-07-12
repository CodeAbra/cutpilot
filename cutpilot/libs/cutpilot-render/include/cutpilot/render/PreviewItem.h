#pragma once

#include "cutpilot/core/CompositePlan.h"

#include <QColor>
#include <QImage>
#include <QQuickRhiItem>
#include <QVector>

namespace cutpilot::render {

// One source image a preview buffer's plan consumes, versioned so the
// renderer re-uploads only when the pixels actually changed.
struct PreviewSource {
    int nodeId = -1;
    QImage image;
    int version = -1;
};

// A preview buffer's whole recipe: the pinned node's composite plan and the
// source pixels it consumes. Inactive buffers draw nothing.
struct PreviewBufferData {
    bool active = false;
    core::CompositePlan plan;
    QVector<PreviewSource> sources;
};

// The preview surface: a GPU item that evaluates the pinned A (and B)
// composite chains through the compositor engine and presents them with the
// compare modes — single, wipe, side-by-side, difference, overlay — over a
// checkerboard, color-managed through the display transform. All state is
// set on the GUI thread and copied to the render thread in synchronize.
class PreviewItem : public QQuickRhiItem {
    Q_OBJECT

public:
    enum class CompareMode {
        Single,
        Wipe,
        SideBySide,
        Difference,
        Overlay
    };
    Q_ENUM(CompareMode)

    explicit PreviewItem(QQuickItem *parent = nullptr);

    // Buffer slots: 0 is A, 1 is B.
    void setBuffer(int slot, const PreviewBufferData &data);
    void clearBuffer(int slot);
    const PreviewBufferData &buffer(int slot) const;

    // Queue a node's GPU resources for release once it leaves every pinned
    // plan; the renderer consumes the queue on its own thread during the
    // next synchronize.
    void releaseNode(int nodeId);
    QVector<int> pendingReleases() const { return m_pendingReleases; }

    void setCompareMode(CompareMode mode);
    CompareMode compareMode() const { return m_mode; }

    // The wipe divider's position across the view, 0..1.
    void setWipePosition(qreal position);
    qreal wipePosition() const { return m_wipe; }

    // The overlay compare's B-over-A opacity, 0..1.
    void setOverlayOpacity(qreal opacity);
    qreal overlayOpacity() const { return m_overlayOpacity; }

    // Fit the content to the view, or show it at one texel per device pixel.
    void setFitToView(bool fit);
    bool fitToView() const { return m_fit; }

    void setSurroundColor(const QColor &color);
    void setDividerColor(const QColor &color);

protected:
    QQuickRhiItemRenderer *createRenderer() override;

private:
    friend class PreviewRenderer;

    PreviewBufferData m_buffers[2];
    QVector<int> m_pendingReleases;
    CompareMode m_mode = CompareMode::Single;
    qreal m_wipe = 0.5;
    qreal m_overlayOpacity = 0.5;
    bool m_fit = true;
    QColor m_surround{ 20, 21, 24 };
    QColor m_divider{ 232, 232, 235, 220 };
};

// Where the compare surfaces land in the view for the given buffer texture
// sizes (empty size: that buffer is inactive). Every mode except
// side-by-side presents both buffers through the one shared rect.
struct PreviewPlacement {
    QRectF rectA; // the shared content rect; A's own half in side-by-side
    QRectF rectB; // B's own half, side-by-side only
};

PreviewPlacement previewPlacement(const QSize &sizeA, const QSize &sizeB,
                                  const QSize &viewSize,
                                  PreviewItem::CompareMode mode, bool fit);

} // namespace cutpilot::render
