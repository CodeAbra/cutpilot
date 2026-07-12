#pragma once

#include <QQuickRhiItem>
#include <QColor>
#include <QPointF>
#include <memory>

QT_BEGIN_NAMESPACE
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;
QT_END_NAMESPACE

namespace cutpilot::render {

// Draws the dotted grid through QRhi as one full-surface pass. This is the only
// class that touches QRhi directly — the thin boundary the rendering decision
// requires, so a Qt version change reviews exactly this file. The full-canvas grid
// is a legitimate one-off pass; the high-count node geometry of later phases uses
// instanced scene-graph materials instead.
class CanvasRenderer : public QQuickRhiItemRenderer {
public:
    CanvasRenderer();
    ~CanvasRenderer() override;

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void synchronize(QQuickRhiItem *item) override;
    void render(QRhiCommandBuffer *cb) override;

private:
    // Uniform block matching grid.frag's std140 layout. The float scalars pack ahead
    // of the vec4 block; _pad restores the 16-byte alignment std140 gives bgCanvas.
    struct alignas(16) Uniforms {
        float viewportSize[2];
        float panPixels[2];
        float zoom;
        float dpr;
        float minorPitch;
        float majorEvery;
        float yUp;
        float _pad[3];
        float bgCanvas[4];
        float gridDot[4];
        float gridDotMajor[4];
    };

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_uniformBuffer;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;

    Uniforms m_uniforms{};
    bool m_uniformsDirty = true;

    // Whether this backend's framebuffer has a bottom-left (y-up) origin, so the grid
    // fragment shader flips gl_FragCoord to the camera's top-left frame only when it
    // must. Set from the live QRhi backend.
    bool m_yUpInFramebuffer = false;
};

} // namespace cutpilot::render
