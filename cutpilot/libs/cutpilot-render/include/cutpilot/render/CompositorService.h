#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace cutpilot::render {

class CompositorEngine;
class NodeLayerItem;

// Keeps card pixels in step with the graph, off the scrub path. Still files
// decode off the GUI thread whenever their path changes — including through
// undo — and each compositing node's thumbnail renders through a windowless
// compositor engine on a debounce, landing as ordinary card media. Without a
// GPU device the thumbnails stay parameter summaries and stills still load.
class CompositorService : public QObject {
    Q_OBJECT

public:
    explicit CompositorService(QObject *parent = nullptr);
    ~CompositorService() override;

    void setLayer(NodeLayerItem *layer);

public slots:
    // Coalesce bursts of edits into one pass shortly after they settle.
    void scheduleRefresh();

signals:
    // A still finished decoding; surfaces reading source pixels (the
    // preview) should refresh.
    void mediaUpdated();

private:
    void refreshNow();
    void reconcileStills();
    void renderThumbnails();

    NodeLayerItem *m_layer = nullptr;
    std::unique_ptr<CompositorEngine> m_engine;
    bool m_engineTried = false;
    QTimer *m_timer = nullptr;

    // What is already on the cards: the decoded path per still node and the
    // rendered plan key per compositing node, so an unchanged node is never
    // re-decoded or re-read-back.
    QHash<int, QString> m_decodedPaths;
    QHash<int, QString> m_thumbnailKeys;
};

} // namespace cutpilot::render
