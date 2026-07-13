#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QThread;
class QTimer;
QT_END_NAMESPACE

namespace cutpilot::core {
struct CompositePlan;
}

namespace cutpilot::render {

class NodeLayerItem;

// Keeps card pixels in step with the graph, off the scrub path. Still files
// decode off the GUI thread whenever their path changes — including through
// undo — and each compositing node's thumbnail renders through a windowless
// compositor engine on its own render thread, landing back here as ordinary
// card media, so a heavy composite never blocks the GUI thread. Without a
// GPU device the thumbnails stay parameter summaries and stills still load.
//
// Video nodes get a playback each: the media stack decodes on its own
// threads, and every delivered frame lands as the node's media at proxy
// resolution, so the transport scrubs without ever blocking the UI thread.
class CompositorService : public QObject {
    Q_OBJECT

public:
    explicit CompositorService(QObject *parent = nullptr);
    ~CompositorService() override;

    void setLayer(NodeLayerItem *layer);

    // The transport of a video node's playback. Scrub positions are a
    // fraction of the duration; both calls return immediately and the
    // sought frame arrives asynchronously.
    void setVideoPlaying(int nodeId, bool playing);
    void scrubVideo(int nodeId, qreal fraction);
    bool videoPlaying(int nodeId) const;
    qint64 videoDurationMs(int nodeId) const;
    qint64 videoPositionMs(int nodeId) const;

public slots:
    // Coalesce bursts of edits into one pass shortly after they settle.
    void scheduleRefresh();

signals:
    // A source's pixels changed — a still finished decoding or a video frame
    // arrived; surfaces reading source pixels (the preview) should refresh.
    void mediaUpdated();

    // A video's position, duration, or play state moved; transport surfaces
    // should re-read theirs.
    void videoStateChanged(int nodeId);

private:
    struct VideoPlayback;
    struct RenderWorker;

    void refreshNow();
    void reconcileStills();
    void reconcileVideos();
    void renderThumbnails();
    void ensureRenderThread();
    void releaseRenderedNode(int nodeId);
    void adoptThumbnail(int nodeId, const QString &key, const QImage &image);

    NodeLayerItem *m_layer = nullptr;
    QTimer *m_timer = nullptr;

    // The thumbnail render thread: a host object living on it receives the
    // queued render jobs, and the worker state — the GPU engine included —
    // is touched only from that thread until the blocking teardown.
    QThread *m_renderThread = nullptr;
    QObject *m_renderHost = nullptr;
    std::shared_ptr<RenderWorker> m_renderWorker;
    bool m_deviceFailed = false;

    // What is already on the cards: the decoded path per still node and the
    // last requested plan key per compositing node, so an unchanged node is
    // never re-decoded or re-rendered and a stale delivery is never adopted.
    QHash<int, QString> m_decodedPaths;
    QHash<int, QString> m_thumbnailKeys;
    QHash<int, VideoPlayback *> m_videos;
};

} // namespace cutpilot::render
