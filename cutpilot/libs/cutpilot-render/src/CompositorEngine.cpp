#include "cutpilot/render/CompositorEngine.h"

#include <rhi/qrhi.h>

#include <QFile>
#include <QtMath>

#include <unordered_map>

namespace cutpilot::render {

namespace {

// Source textures are clamped to this dimension so an oversized still can
// never exhaust GPU memory; the preview is a display surface, not an export.
constexpr int kMaxSourceDim = 2048;

QShader loadShader(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    return QShader();
}

// std140 mirrors of the composite fragment shaders' uniform blocks.
struct BlendUniforms {
    qint32 mode;
    float opacity;
    qint32 hasBase;
    qint32 hasOver;
    qint32 baseMatte;
    qint32 overMatte;
    float pad[2];
};

struct MaskUniforms {
    qint32 invertMask;
    qint32 hasImage;
    qint32 imageMatte;
    qint32 hasMask;
    qint32 maskMatte;
    float pad[3];
};

struct KeyUniforms {
    float keyColor[4];
    qint32 lumaKey;
    float tolerance;
    float softness;
    qint32 hasImage;
    qint32 imageMatte;
    float pad[3];
};

struct TransformUniforms {
    float translate[2];
    float texSize[2];
    float scale;
    float rotationRad;
    qint32 hasImage;
    qint32 imageMatte;
};

constexpr int kUniformCapacity = 64;
static_assert(sizeof(BlendUniforms) <= kUniformCapacity);
static_assert(sizeof(MaskUniforms) <= kUniformCapacity);
static_assert(sizeof(KeyUniforms) <= kUniformCapacity);
static_assert(sizeof(TransformUniforms) <= kUniformCapacity);

} // namespace

struct CompositorEngine::Impl {
    struct Source {
        std::unique_ptr<QRhiTexture> texture;
        int version = -1;
        QImage pendingUpload; // consumed by the next evaluate
    };

    struct Pass {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiTextureRenderTarget> target;
        std::unique_ptr<QRhiRenderPassDescriptor> targetPass;
        std::unique_ptr<QRhiBuffer> uniforms;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        QString cachedKey;
    };

    QRhi *rhi = nullptr;
    std::unique_ptr<QRhi> ownedRhi;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> dummy;
    bool dummyUploaded = false;
    std::unique_ptr<QRhiBuffer> layoutUniforms;
    std::unordered_map<int, std::unique_ptr<QRhiGraphicsPipeline>> pipelines;
    std::unordered_map<int, std::unique_ptr<QRhiShaderResourceBindings>> layouts;
    std::unordered_map<int, Source> sources;
    std::unordered_map<int, Pass> passes;
    int lastPassCount = 0;

    void releaseAll()
    {
        passes.clear();
        for (auto &entry : sources) {
            entry.second.texture.reset();
            entry.second.version = -1;
        }
        pipelines.clear();
        layouts.clear();
        layoutUniforms.reset();
        dummy.reset();
        dummyUploaded = false;
        sampler.reset();
    }

    static int samplerCount(core::NodeKind kind)
    {
        return (kind == core::NodeKind::Blend || kind == core::NodeKind::Mask) ? 2
                                                                               : 1;
    }

    static QString fragmentShaderPath(core::NodeKind kind)
    {
        switch (kind) {
        case core::NodeKind::Blend:
            return QStringLiteral(":/cutpilot/render/shaders/blend.frag.qsb");
        case core::NodeKind::Mask:
            return QStringLiteral(":/cutpilot/render/shaders/mask.frag.qsb");
        case core::NodeKind::Key:
            return QStringLiteral(":/cutpilot/render/shaders/key.frag.qsb");
        case core::NodeKind::Transform:
            return QStringLiteral(":/cutpilot/render/shaders/transform.frag.qsb");
        default:
            return QString();
        }
    }

    bool ensureStatics()
    {
        if (!rhi)
            return false;
        if (!sampler) {
            sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                          QRhiSampler::None,
                                          QRhiSampler::ClampToEdge,
                                          QRhiSampler::ClampToEdge));
            if (!sampler->create())
                return false;
        }
        if (!dummy) {
            dummy.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
            if (!dummy->create())
                return false;
            dummyUploaded = false;
        }
        if (!layoutUniforms) {
            layoutUniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic,
                                                QRhiBuffer::UniformBuffer,
                                                kUniformCapacity));
            if (!layoutUniforms->create())
                return false;
        }
        return true;
    }

    // The persistent layout-defining bindings a kind's pipeline is built
    // against; per-pass bindings mirror this layout with real resources.
    QRhiShaderResourceBindings *layoutFor(core::NodeKind kind)
    {
        auto it = layouts.find(int(kind));
        if (it != layouts.end())
            return it->second.get();

        QList<QRhiShaderResourceBinding> bindings;
        bindings.append(QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, layoutUniforms.get()));
        for (int i = 0; i < samplerCount(kind); ++i) {
            bindings.append(QRhiShaderResourceBinding::sampledTexture(
                1 + i, QRhiShaderResourceBinding::FragmentStage, dummy.get(),
                sampler.get()));
        }
        auto srb = std::unique_ptr<QRhiShaderResourceBindings>(
            rhi->newShaderResourceBindings());
        srb->setBindings(bindings.cbegin(), bindings.cend());
        if (!srb->create())
            return nullptr;
        return layouts.emplace(int(kind), std::move(srb)).first->second.get();
    }

    QRhiGraphicsPipeline *pipelineFor(core::NodeKind kind,
                                      QRhiRenderPassDescriptor *targetPass)
    {
        auto it = pipelines.find(int(kind));
        if (it != pipelines.end())
            return it->second.get();

        QRhiShaderResourceBindings *layout = layoutFor(kind);
        if (!layout)
            return nullptr;

        auto pipeline =
            std::unique_ptr<QRhiGraphicsPipeline>(rhi->newGraphicsPipeline());
        pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,
              loadShader(
                  QStringLiteral(":/cutpilot/render/shaders/composite.vert.qsb")) },
            { QRhiShaderStage::Fragment, loadShader(fragmentShaderPath(kind)) },
        });
        // The fragment shader computes the final straight-alpha value, so the
        // target is plainly overwritten — no fixed-function blending.
        QRhiVertexInputLayout inputLayout;
        pipeline->setVertexInputLayout(inputLayout);
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setShaderResourceBindings(layout);
        pipeline->setRenderPassDescriptor(targetPass);
        if (!pipeline->create())
            return nullptr;
        return pipelines.emplace(int(kind), std::move(pipeline)).first->second.get();
    }

    // The texture a pass input reads: an upstream pass's output, a source's
    // upload, or null when unwired or not yet arrived.
    QRhiTexture *inputTexture(int nodeId) const
    {
        if (nodeId == -1)
            return nullptr;
        const auto pass = passes.find(nodeId);
        if (pass != passes.end() && pass->second.texture)
            return pass->second.texture.get();
        const auto source = sources.find(nodeId);
        if (source != sources.end() && source->second.texture
            && source->second.version >= 0)
            return source->second.texture.get();
        return nullptr;
    }

    QSize targetSizeFor(const core::CompositePass &pass)
    {
        for (const core::CompositeInput &input : pass.inputs) {
            if (QRhiTexture *tex = inputTexture(input.nodeId))
                return tex->pixelSize();
        }
        return QSize(64, 64);
    }

    void fillUniforms(const core::CompositePass &pass, const QSize &targetSize,
                      char (&data)[kUniformCapacity])
    {
        const core::CompositeParams &p = pass.params;
        const auto has = [this, &pass](int index) -> qint32 {
            return index < pass.inputs.size()
                    && inputTexture(pass.inputs[index].nodeId)
                ? 1
                : 0;
        };
        const auto matte = [&pass](int index) -> qint32 {
            return index < pass.inputs.size() && pass.inputs[index].matte ? 1 : 0;
        };

        memset(data, 0, sizeof(data));
        switch (pass.kind) {
        case core::NodeKind::Blend: {
            BlendUniforms u{};
            u.mode = int(p.blendMode);
            u.opacity = float(p.opacity);
            u.hasBase = has(0);
            u.hasOver = has(1);
            u.baseMatte = matte(0);
            u.overMatte = matte(1);
            memcpy(data, &u, sizeof(u));
            break;
        }
        case core::NodeKind::Mask: {
            MaskUniforms u{};
            u.invertMask = p.invertMask ? 1 : 0;
            u.hasImage = has(0);
            u.imageMatte = matte(0);
            u.hasMask = has(1);
            u.maskMatte = matte(1);
            memcpy(data, &u, sizeof(u));
            break;
        }
        case core::NodeKind::Key: {
            KeyUniforms u{};
            u.keyColor[0] = float(p.keyColor.redF());
            u.keyColor[1] = float(p.keyColor.greenF());
            u.keyColor[2] = float(p.keyColor.blueF());
            u.keyColor[3] = 1.0f;
            u.lumaKey = p.lumaKey ? 1 : 0;
            u.tolerance = float(p.keyTolerance);
            u.softness = float(p.keySoftness);
            u.hasImage = has(0);
            u.imageMatte = matte(0);
            memcpy(data, &u, sizeof(u));
            break;
        }
        case core::NodeKind::Transform: {
            TransformUniforms u{};
            u.translate[0] = float(p.translateX);
            u.translate[1] = float(p.translateY);
            u.texSize[0] = float(targetSize.width());
            u.texSize[1] = float(targetSize.height());
            u.scale = float(p.scale);
            u.rotationRad = float(qDegreesToRadians(p.rotationDeg));
            u.hasImage = has(0);
            u.imageMatte = matte(0);
            memcpy(data, &u, sizeof(u));
            break;
        }
        default:
            break;
        }
    }
};

CompositorEngine::CompositorEngine()
    : d(std::make_unique<Impl>())
{
}

CompositorEngine::~CompositorEngine() = default;

void CompositorEngine::setRhi(QRhi *rhi)
{
    if (d->rhi == rhi)
        return;
    d->releaseAll();
    d->ownedRhi.reset();
    d->rhi = rhi;
}

QRhi *CompositorEngine::rhi() const
{
    return d->rhi;
}

bool CompositorEngine::adoptHeadlessDevice()
{
    std::unique_ptr<QRhi> device;
#ifdef Q_OS_MACOS
    QRhiMetalInitParams params;
    device.reset(QRhi::create(QRhi::Metal, &params));
#endif
    if (!device)
        return false;
    d->releaseAll();
    d->rhi = device.get();
    d->ownedRhi = std::move(device);
    return true;
}

void CompositorEngine::setSource(int nodeId, const QImage &image, int version)
{
    Impl::Source &source = d->sources[nodeId];
    if (source.version == version)
        return;

    QImage prepared = image;
    if (prepared.width() > kMaxSourceDim || prepared.height() > kMaxSourceDim)
        prepared = prepared.scaled(kMaxSourceDim, kMaxSourceDim,
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (prepared.format() != QImage::Format_RGBA8888)
        prepared = prepared.convertToFormat(QImage::Format_RGBA8888);

    source.pendingUpload = prepared;
    source.version = version;
}

int CompositorEngine::sourceVersion(int nodeId) const
{
    const auto it = d->sources.find(nodeId);
    return it != d->sources.end() ? it->second.version : -1;
}

void CompositorEngine::releaseNode(int nodeId)
{
    d->sources.erase(nodeId);
    d->passes.erase(nodeId);
}

QRhiTexture *CompositorEngine::evaluate(const core::CompositePlan &plan,
                                        QRhiCommandBuffer *cb)
{
    d->lastPassCount = 0;
    if (!d->rhi || !cb || !plan.valid || !d->ensureStatics())
        return nullptr;

    // Pending source uploads and the one-time dummy clear travel on the
    // first recorded batch; if everything is cached they still need a home.
    QRhiResourceUpdateBatch *uploads = d->rhi->nextResourceUpdateBatch();
    if (!d->dummyUploaded) {
        QImage clear(1, 1, QImage::Format_RGBA8888);
        clear.fill(Qt::transparent);
        uploads->uploadTexture(d->dummy.get(), clear);
        d->dummyUploaded = true;
    }
    for (int nodeId : plan.sourceNodeIds) {
        auto it = d->sources.find(nodeId);
        if (it == d->sources.end() || it->second.pendingUpload.isNull())
            continue;
        Impl::Source &source = it->second;
        const QSize size = source.pendingUpload.size();
        if (!source.texture || source.texture->pixelSize() != size) {
            source.texture.reset(d->rhi->newTexture(QRhiTexture::RGBA8, size));
            if (!source.texture->create()) {
                source.texture.reset();
                continue;
            }
        }
        uploads->uploadTexture(source.texture.get(), source.pendingUpload);
        source.pendingUpload = QImage();
    }

    // Any source version bump invalidates the plan's cached passes: pixels
    // may have changed under an unchanged wiring signature (a result file
    // arriving after its digest was already known).
    QString sourceStamp;
    for (int nodeId : plan.sourceNodeIds) {
        sourceStamp += QString::number(nodeId);
        sourceStamp += QLatin1Char(':');
        sourceStamp += QString::number(sourceVersion(nodeId));
        sourceStamp += QLatin1Char('|');
    }

    // A device-level create failing below abandons the evaluation; the
    // un-consumed batch must go back to the rhi rather than leak.
    const auto fail = [&uploads]() -> QRhiTexture * {
        if (uploads)
            uploads->release();
        return nullptr;
    };

    QRhiTexture *result = nullptr;
    for (const core::CompositePass &planPass : plan.passes) {
        Impl::Pass &pass = d->passes[planPass.nodeId];
        const QSize size = d->targetSizeFor(planPass);
        const QString key = planPass.signature + QLatin1Char('#') + sourceStamp
            + QString::number(size.width()) + QLatin1Char('x')
            + QString::number(size.height());

        if (pass.cachedKey == key && pass.texture) {
            if (uploads) {
                // Nothing rendered yet still commits the uploads.
                cb->resourceUpdate(uploads);
                uploads = nullptr;
            }
            result = pass.texture.get();
            continue;
        }

        if (!pass.texture || pass.texture->pixelSize() != size) {
            pass.texture.reset(d->rhi->newTexture(
                QRhiTexture::RGBA8, size, 1,
                QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
            if (!pass.texture->create()) {
                pass.texture.reset();
                return fail();
            }
            QRhiColorAttachment attachment(pass.texture.get());
            pass.target.reset(d->rhi->newTextureRenderTarget({ attachment }));
            pass.targetPass.reset(pass.target->newCompatibleRenderPassDescriptor());
            pass.target->setRenderPassDescriptor(pass.targetPass.get());
            if (!pass.target->create())
                return fail();
        }
        if (!pass.uniforms) {
            pass.uniforms.reset(d->rhi->newBuffer(
                QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformCapacity));
            if (!pass.uniforms->create())
                return fail();
        }

        QRhiGraphicsPipeline *pipeline =
            d->pipelineFor(planPass.kind, pass.targetPass.get());
        if (!pipeline)
            return fail();

        // Per-pass bindings carry this pass's actual inputs; unwired or
        // not-yet-arrived inputs fall back to the transparent dummy and the
        // shader's has-flags ignore them.
        QList<QRhiShaderResourceBinding> bindings;
        bindings.append(QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::FragmentStage, pass.uniforms.get()));
        for (int i = 0; i < Impl::samplerCount(planPass.kind); ++i) {
            QRhiTexture *input = i < planPass.inputs.size()
                ? d->inputTexture(planPass.inputs[i].nodeId)
                : nullptr;
            bindings.append(QRhiShaderResourceBinding::sampledTexture(
                1 + i, QRhiShaderResourceBinding::FragmentStage,
                input ? input : d->dummy.get(), d->sampler.get()));
        }
        pass.bindings.reset(d->rhi->newShaderResourceBindings());
        pass.bindings->setBindings(bindings.cbegin(), bindings.cend());
        if (!pass.bindings->create())
            return fail();

        char uniformData[kUniformCapacity];
        d->fillUniforms(planPass, size, uniformData);
        QRhiResourceUpdateBatch *batch =
            uploads ? uploads : d->rhi->nextResourceUpdateBatch();
        uploads = nullptr;
        batch->updateDynamicBuffer(pass.uniforms.get(), 0, kUniformCapacity,
                                   uniformData);

        cb->beginPass(pass.target.get(), Qt::transparent, { 1.0f, 0 }, batch);
        cb->setGraphicsPipeline(pipeline);
        cb->setViewport(
            QRhiViewport(0, 0, float(size.width()), float(size.height())));
        cb->setShaderResources(pass.bindings.get());
        cb->draw(3);
        cb->endPass();

        pass.cachedKey = key;
        ++d->lastPassCount;
        result = pass.texture.get();
    }

    if (uploads)
        cb->resourceUpdate(uploads);

    // The plan's last pass is the target; an empty pass list means the
    // target itself is a source.
    if (!plan.passes.isEmpty())
        return result;
    return d->inputTexture(plan.targetNodeId);
}

QImage CompositorEngine::evaluateToImage(const core::CompositePlan &plan)
{
    if (!d->rhi)
        return QImage();

    QRhiCommandBuffer *cb = nullptr;
    if (d->rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return QImage();

    QRhiTexture *texture = evaluate(plan, cb);
    QRhiReadbackResult readback;
    if (texture) {
        QRhiResourceUpdateBatch *batch = d->rhi->nextResourceUpdateBatch();
        batch->readBackTexture(QRhiReadbackDescription(texture), &readback);
        cb->resourceUpdate(batch);
    }
    d->rhi->endOffscreenFrame();

    if (!texture || readback.data.isEmpty())
        return QImage();

    QImage image(reinterpret_cast<const uchar *>(readback.data.constData()),
                 readback.pixelSize.width(), readback.pixelSize.height(),
                 QImage::Format_RGBA8888);
    return image.copy();
}

int CompositorEngine::lastPassCount() const
{
    return d->lastPassCount;
}

} // namespace cutpilot::render
