#include "CanvasRenderer.h"
#include "cutpilot/render/CanvasItem.h"

#include <rhi/qrhi.h>

#include <QFile>
#include <QQuickWindow>

#include <cstring>

namespace cutpilot::render {

namespace {

QShader loadShader(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    return QShader();
}

void fillColor(float (&dst)[4], const QColor &c)
{
    dst[0] = float(c.redF());
    dst[1] = float(c.greenF());
    dst[2] = float(c.blueF());
    dst[3] = float(c.alphaF());
}

} // namespace

CanvasRenderer::CanvasRenderer() = default;
CanvasRenderer::~CanvasRenderer() = default;

void CanvasRenderer::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        m_rhi = rhi();
        m_yUpInFramebuffer = m_rhi->isYUpInFramebuffer();
        m_pipeline.reset();
        m_srb.reset();
        m_uniformBuffer.reset();
    }

    if (m_pipeline)
        return;

    m_uniformBuffer.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                           QRhiBuffer::UniformBuffer,
                                           sizeof(Uniforms)));
    m_uniformBuffer->create();

    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage
                | QRhiShaderResourceBinding::FragmentStage,
            m_uniformBuffer.get()),
    });
    m_srb->create();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex,
          loadShader(QStringLiteral(":/cutpilot/render/shaders/grid.vert.qsb")) },
        { QRhiShaderStage::Fragment,
          loadShader(QStringLiteral(":/cutpilot/render/shaders/grid.frag.qsb")) },
    });

    // No vertex buffer: the full-surface triangle is generated from the vertex
    // index, so the input layout is empty.
    QRhiVertexInputLayout inputLayout;
    m_pipeline->setVertexInputLayout(inputLayout);
    m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_pipeline->create();

    m_uniformsDirty = true;
}

void CanvasRenderer::synchronize(QQuickRhiItem *rhiItem)
{
    auto *item = static_cast<CanvasItem *>(rhiItem);
    const auto &theme = item->themeTable();

    const QSize px = renderTarget()->pixelSize();
    const qreal dpr = item->window() ? item->window()->effectiveDevicePixelRatio()
                                     : 1.0;

    Uniforms u{};
    u.viewportSize[0] = float(px.width());
    u.viewportSize[1] = float(px.height());
    u.panPixels[0] = float(item->panPixels().x());
    u.panPixels[1] = float(item->panPixels().y());
    u.zoom = float(item->zoom());
    u.dpr = float(dpr);
    u.minorPitch = 24.0f;
    u.majorEvery = 4.0f;
    u.yUp = m_yUpInFramebuffer ? 1.0f : 0.0f;
    fillColor(u.bgCanvas, theme.bgCanvas());
    fillColor(u.gridDot, theme.gridDot());
    fillColor(u.gridDotMajor, theme.gridDotMajor());

    if (std::memcmp(&u, &m_uniforms, sizeof(Uniforms)) != 0) {
        m_uniforms = u;
        m_uniformsDirty = true;
    }
}

void CanvasRenderer::render(QRhiCommandBuffer *cb)
{
    QRhiResourceUpdateBatch *batch = nullptr;
    if (m_uniformsDirty) {
        batch = m_rhi->nextResourceUpdateBatch();
        batch->updateDynamicBuffer(m_uniformBuffer.get(), 0, sizeof(Uniforms),
                                   &m_uniforms);
        m_uniformsDirty = false;
    }

    const QColor clear = m_uniforms.zoom > 0.0f
        ? QColor::fromRgbF(m_uniforms.bgCanvas[0], m_uniforms.bgCanvas[1],
                           m_uniforms.bgCanvas[2])
        : QColor(0x14, 0x15, 0x18);

    const QSize px = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), clear, { 1.0f, 0 }, batch);

    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, float(px.width()), float(px.height())));
    cb->setShaderResources(m_srb.get());
    cb->draw(3);

    cb->endPass();
}

} // namespace cutpilot::render
