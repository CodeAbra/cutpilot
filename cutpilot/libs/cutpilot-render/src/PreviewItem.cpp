#include "cutpilot/render/PreviewItem.h"

#include "cutpilot/render/CompositorEngine.h"

#include <rhi/qrhi.h>

#include <QFile>

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

void fillRect(float (&dst)[4], const QRectF &r)
{
    dst[0] = float(r.x());
    dst[1] = float(r.y());
    dst[2] = float(r.width());
    dst[3] = float(r.height());
}

// std140 mirror of preview.frag's uniform block.
struct PreviewUniforms {
    float rectA[4];
    float rectB[4];
    float surround[4];
    float divider[4];
    float viewSize[2];
    float wipe;
    float overlayOpacity;
    qint32 mode;
    qint32 hasA;
    qint32 hasB;
    float checker;
};
static_assert(sizeof(PreviewUniforms) == 96);

// Where a texture of the given size lands inside the bounds: aspect-fit, or
// centered at one texel per target pixel.
QRectF placeContent(const QSize &textureSize, const QRectF &bounds, bool fit)
{
    if (textureSize.isEmpty() || bounds.isEmpty())
        return QRectF();
    if (!fit) {
        return QRectF(bounds.center()
                          - QPointF(textureSize.width() / 2.0,
                                    textureSize.height() / 2.0),
                      QSizeF(textureSize));
    }
    const qreal scale = qMin(bounds.width() / textureSize.width(),
                             bounds.height() / textureSize.height());
    const QSizeF fitted = QSizeF(textureSize) * scale;
    return QRectF(bounds.center()
                      - QPointF(fitted.width() / 2.0, fitted.height() / 2.0),
                  fitted);
}

} // namespace

// The GPU half of the preview: evaluates both buffers' plans through the
// compositor engine on the scene graph's device, then records the present
// pass into the item's render target. Part of the QRhi boundary reviewed on
// every Qt upgrade.
class PreviewRenderer : public QQuickRhiItemRenderer {
protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void synchronize(QQuickRhiItem *item) override;
    void render(QRhiCommandBuffer *cb) override;

private:
    bool ensureStatics();

    QRhi *m_rhi = nullptr;
    CompositorEngine m_engine;

    std::unique_ptr<QRhiBuffer> m_uniforms;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiTexture> m_dummy;
    bool m_dummyUploaded = false;
    std::unique_ptr<QRhiShaderResourceBindings> m_bindings;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    QRhiTexture *m_boundA = nullptr;
    QRhiTexture *m_boundB = nullptr;

    // State copied from the item during synchronize.
    PreviewBufferData m_buffers[2];
    PreviewItem::CompareMode m_mode = PreviewItem::CompareMode::Single;
    float m_wipe = 0.5f;
    float m_overlayOpacity = 0.5f;
    bool m_fit = true;
    QColor m_surround;
    QColor m_divider;
};

void PreviewRenderer::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        m_rhi = rhi();
        m_pipeline.reset();
        m_bindings.reset();
        m_uniforms.reset();
        m_sampler.reset();
        m_dummy.reset();
        m_dummyUploaded = false;
        m_boundA = nullptr;
        m_boundB = nullptr;
        m_engine.setRhi(m_rhi);
    }
}

bool PreviewRenderer::ensureStatics()
{
    if (!m_uniforms) {
        m_uniforms.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic,
                                          QRhiBuffer::UniformBuffer,
                                          sizeof(PreviewUniforms)));
        if (!m_uniforms->create())
            return false;
    }
    if (!m_sampler) {
        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                          QRhiSampler::None,
                                          QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge));
        if (!m_sampler->create())
            return false;
    }
    if (!m_dummy) {
        m_dummy.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        if (!m_dummy->create())
            return false;
        m_dummyUploaded = false;
    }
    return true;
}

void PreviewRenderer::synchronize(QQuickRhiItem *rhiItem)
{
    auto *item = static_cast<PreviewItem *>(rhiItem);
    m_mode = item->compareMode();
    m_wipe = float(item->wipePosition());
    m_overlayOpacity = float(item->overlayOpacity());
    m_fit = item->fitToView();
    m_surround = item->m_surround;
    m_divider = item->m_divider;

    for (int slot = 0; slot < 2; ++slot) {
        m_buffers[slot] = item->m_buffers[slot];
        for (const PreviewSource &source : m_buffers[slot].sources)
            m_engine.setSource(source.nodeId, source.image, source.version);
    }
}

void PreviewRenderer::render(QRhiCommandBuffer *cb)
{
    const QSize outputSize = renderTarget()->pixelSize();
    if (outputSize.isEmpty())
        return;
    if (!ensureStatics()) {
        cb->beginPass(renderTarget(), m_surround, { 1.0f, 0 });
        cb->endPass();
        return;
    }

    QRhiTexture *texA = m_buffers[0].active
        ? m_engine.evaluate(m_buffers[0].plan, cb)
        : nullptr;
    QRhiTexture *texB = m_buffers[1].active
        ? m_engine.evaluate(m_buffers[1].plan, cb)
        : nullptr;

    const bool sideBySide = m_mode == PreviewItem::CompareMode::SideBySide;
    const QRectF full(QPointF(0, 0), QSizeF(outputSize));
    const QRectF halfA(0, 0, outputSize.width() / 2.0, outputSize.height());
    const QRectF halfB(outputSize.width() / 2.0, 0, outputSize.width() / 2.0,
                       outputSize.height());
    const QRectF rectA = texA
        ? placeContent(texA->pixelSize(), sideBySide ? halfA : full, m_fit)
        : QRectF();
    const QRectF rectB = (texB && sideBySide)
        ? placeContent(texB->pixelSize(), halfB, m_fit)
        : QRectF();

    PreviewUniforms u{};
    fillRect(u.rectA, rectA);
    fillRect(u.rectB, rectB);
    fillColor(u.surround, m_surround);
    fillColor(u.divider, m_divider);
    u.viewSize[0] = float(outputSize.width());
    u.viewSize[1] = float(outputSize.height());
    u.wipe = m_wipe;
    u.overlayOpacity = m_overlayOpacity;
    u.mode = int(m_mode);
    u.hasA = texA ? 1 : 0;
    u.hasB = texB ? 1 : 0;
    u.checker = 12.0f;

    QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();
    if (!m_dummyUploaded) {
        QImage clear(1, 1, QImage::Format_RGBA8888);
        clear.fill(Qt::transparent);
        batch->uploadTexture(m_dummy.get(), clear);
        m_dummyUploaded = true;
    }
    batch->updateDynamicBuffer(m_uniforms.get(), 0, sizeof(PreviewUniforms), &u);

    QRhiTexture *bindA = texA ? texA : m_dummy.get();
    QRhiTexture *bindB = texB ? texB : m_dummy.get();
    if (!m_bindings || m_boundA != bindA || m_boundB != bindB) {
        m_bindings.reset(m_rhi->newShaderResourceBindings());
        m_bindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::FragmentStage, m_uniforms.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage, bindA,
                m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage, bindB,
                m_sampler.get()),
        });
        if (!m_bindings->create()) {
            batch->release();
            return;
        }
        m_boundA = bindA;
        m_boundB = bindB;
    }

    if (!m_pipeline) {
        m_pipeline.reset(m_rhi->newGraphicsPipeline());
        m_pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,
              loadShader(
                  QStringLiteral(":/cutpilot/render/shaders/composite.vert.qsb")) },
            { QRhiShaderStage::Fragment,
              loadShader(
                  QStringLiteral(":/cutpilot/render/shaders/preview.frag.qsb")) },
        });
        QRhiVertexInputLayout inputLayout;
        m_pipeline->setVertexInputLayout(inputLayout);
        m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        m_pipeline->setShaderResourceBindings(m_bindings.get());
        m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        if (!m_pipeline->create()) {
            m_pipeline.reset();
            batch->release();
            return;
        }
    }

    cb->beginPass(renderTarget(), m_surround, { 1.0f, 0 }, batch);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(
        QRhiViewport(0, 0, float(outputSize.width()), float(outputSize.height())));
    cb->setShaderResources(m_bindings.get());
    cb->draw(3);
    cb->endPass();
}

PreviewItem::PreviewItem(QQuickItem *parent)
    : QQuickRhiItem(parent)
{
}

QQuickRhiItemRenderer *PreviewItem::createRenderer()
{
    return new PreviewRenderer;
}

void PreviewItem::setBuffer(int slot, const PreviewBufferData &data)
{
    if (slot < 0 || slot > 1)
        return;
    m_buffers[slot] = data;
    update();
}

void PreviewItem::clearBuffer(int slot)
{
    if (slot < 0 || slot > 1)
        return;
    m_buffers[slot] = PreviewBufferData();
    update();
}

const PreviewBufferData &PreviewItem::buffer(int slot) const
{
    return m_buffers[qBound(0, slot, 1)];
}

void PreviewItem::setCompareMode(CompareMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    update();
}

void PreviewItem::setWipePosition(qreal position)
{
    const qreal clamped = qBound(0.0, position, 1.0);
    if (m_wipe == clamped)
        return;
    m_wipe = clamped;
    update();
}

void PreviewItem::setOverlayOpacity(qreal opacity)
{
    const qreal clamped = qBound(0.0, opacity, 1.0);
    if (m_overlayOpacity == clamped)
        return;
    m_overlayOpacity = clamped;
    update();
}

void PreviewItem::setFitToView(bool fit)
{
    if (m_fit == fit)
        return;
    m_fit = fit;
    update();
}

void PreviewItem::setSurroundColor(const QColor &color)
{
    if (m_surround == color)
        return;
    m_surround = color;
    update();
}

void PreviewItem::setDividerColor(const QColor &color)
{
    if (m_divider == color)
        return;
    m_divider = color;
    update();
}

} // namespace cutpilot::render
