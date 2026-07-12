#include "cutpilot/render/CompositorService.h"

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/CompositePlan.h"
#include "cutpilot/render/CompositorEngine.h"
#include "cutpilot/render/NodeLayerItem.h"

#include <QFutureWatcher>
#include <QImage>
#include <QImageReader>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace cutpilot::render {

namespace {

constexpr int kDebounceMs = 150;

// Thumbnails render at card scale; sources are downscaled before upload so a
// large still never costs full-resolution passes twice.
constexpr int kThumbnailSourceDim = 512;

QImage decodeBounded(const QString &path)
{
    QImageReader reader(path);
    reader.setAllocationLimit(256); // megabytes; a card image, not an export
    return reader.read();
}

} // namespace

CompositorService::CompositorService(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(kDebounceMs);
    connect(m_timer, &QTimer::timeout, this, &CompositorService::refreshNow);
}

CompositorService::~CompositorService() = default;

void CompositorService::setLayer(NodeLayerItem *layer)
{
    if (m_layer == layer)
        return;
    if (m_layer)
        m_layer->disconnect(this);
    m_layer = layer;
    if (m_layer) {
        connect(m_layer, &NodeLayerItem::graphMutated, this,
                &CompositorService::scheduleRefresh);
        scheduleRefresh();
    }
}

void CompositorService::scheduleRefresh()
{
    m_timer->start();
}

void CompositorService::refreshNow()
{
    if (!m_layer)
        return;

    // Forget nodes that left the graph, so an undo that restores them is a
    // fresh decode/render rather than a stale hit.
    const auto prune = [this](QHash<int, QString> &tracked) {
        for (auto it = tracked.begin(); it != tracked.end();) {
            if (!m_layer->graph().nodeById(it.key())) {
                if (m_engine)
                    m_engine->releaseNode(it.key());
                it = tracked.erase(it);
            } else {
                ++it;
            }
        }
    };
    prune(m_decodedPaths);
    prune(m_thumbnailKeys);

    reconcileStills();
    renderThumbnails();
}

void CompositorService::reconcileStills()
{
    QVector<int> stillIds;
    for (const core::Node &node : m_layer->graph().nodes()) {
        if (node.kind == core::NodeKind::Still)
            stillIds.push_back(node.id);
    }

    for (int nodeId : stillIds) {
        core::Node *node = m_layer->graph().nodeById(nodeId);
        const QString path = node->mediaPath;
        if (m_decodedPaths.value(nodeId) == path)
            continue;
        m_decodedPaths.insert(nodeId, path);

        if (path.isEmpty()) {
            m_layer->clearNodeMedia(nodeId);
            continue;
        }

        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this,
                [this, watcher, nodeId, path] {
                    watcher->deleteLater();
                    core::Node *node = m_layer->graph().nodeById(nodeId);
                    if (!node || node->mediaPath != path)
                        return; // the path moved on while decoding
                    const QImage image = watcher->result();
                    if (image.isNull()) {
                        node->statusMessage =
                            QStringLiteral("Could not read the image");
                        node->bumpContent();
                        m_layer->clearNodeMedia(nodeId);
                        m_layer->refreshNode(nodeId);
                        return;
                    }
                    if (!node->statusMessage.isEmpty()) {
                        node->statusMessage.clear();
                        node->bumpContent();
                    }
                    m_layer->setNodeMedia(nodeId, image);
                    emit mediaUpdated();
                    scheduleRefresh(); // downstream thumbnails now have pixels
                });
        watcher->setFuture(QtConcurrent::run(decodeBounded, path));
    }
}

void CompositorService::renderThumbnails()
{
    QVector<int> compositeIds;
    for (const core::Node &node : m_layer->graph().nodes()) {
        if (core::isCompositeKind(node.kind))
            compositeIds.push_back(node.id);
    }
    if (compositeIds.isEmpty())
        return;

    if (!m_engine && !m_engineTried) {
        m_engineTried = true;
        auto engine = std::make_unique<CompositorEngine>();
        if (engine->adoptHeadlessDevice())
            m_engine = std::move(engine);
        else
            qWarning("Compositor thumbnails disabled: no windowless GPU device");
    }
    if (!m_engine)
        return;

    for (int nodeId : compositeIds) {
        const core::CompositePlan plan =
            core::buildCompositePlan(m_layer->graph(), nodeId);
        if (!plan.valid || plan.passes.isEmpty()) {
            m_thumbnailKeys.remove(nodeId);
            m_layer->clearNodeMedia(nodeId);
            continue;
        }

        QString key = plan.passes.last().signature;
        for (int sourceId : plan.sourceNodeIds) {
            key += QLatin1Char('|');
            key += QString::number(sourceId);
            key += QLatin1Char(':');
            key += QString::number(m_layer->nodeMediaVersion(sourceId));
        }
        if (m_thumbnailKeys.value(nodeId) == key)
            continue;

        for (int sourceId : plan.sourceNodeIds) {
            QImage image = m_layer->nodeMediaImage(sourceId);
            if (image.isNull())
                continue;
            if (image.width() > kThumbnailSourceDim
                || image.height() > kThumbnailSourceDim) {
                image = image.scaled(kThumbnailSourceDim, kThumbnailSourceDim,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            m_engine->setSource(sourceId, image,
                                m_layer->nodeMediaVersion(sourceId));
        }

        const QImage thumbnail = m_engine->evaluateToImage(plan);
        m_thumbnailKeys.insert(nodeId, key);
        if (!thumbnail.isNull())
            m_layer->setNodeMedia(nodeId, thumbnail);
        else
            m_layer->clearNodeMedia(nodeId);
    }
}

} // namespace cutpilot::render
