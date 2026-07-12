#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include "cutpilot/ipc/GenerationTypes.h"

namespace cutpilot::core {
class NodeGraph;
struct Node;
}

namespace cutpilot::ipc {

class GenerationClient;

// Drives generation runs for the node graph: resolves a node's prompt and
// model, submits the job, and writes the streamed status back onto the node
// so the canvas can draw it. Everything happens on this object's thread; the
// slow work stays in the service, so writing status is the only graph
// mutation and the UI thread never waits on a vendor.
//
// Prompt resolution order: the node wired into the generate node's text input
// supplies the prompt; without a wire the node's own prompt text is used; an
// empty prompt refuses the run with guidance instead of submitting.
class GenerationCoordinator : public QObject {
    Q_OBJECT

public:
    GenerationCoordinator(core::NodeGraph *graph, GenerationClient *client,
                          QObject *parent = nullptr);

    // Called once the service is health-gated; loads the model registry and
    // gives model-less generate nodes the default entry.
    void serviceBecameReady();
    void serviceBecameUnavailable(const QString &reason);

    bool serviceReady() const { return m_serviceReady; }
    const QVector<ModelInfo> &models() const { return m_models; }

    void runNode(int nodeId);
    void stopNode(int nodeId);

    // Drop run state that no longer matches reality: nodes claiming to run
    // without a live job fall back to idle, and jobs whose node vanished are
    // canceled. Called after any structural edit or history walk.
    void reconcile();

signals:
    // The node's textual/status content changed and its card should refresh.
    void nodeContentChanged(int nodeId);

    // The finished result decoded off the GUI thread and is ready to display.
    void nodeMediaReady(int nodeId, const QImage &image);

    void modelsReady();
    void addKeyNeeded(int nodeId, const QString &provider);

private:
    core::Node *generateNode(int nodeId);
    QString resolvePrompt(const core::Node &node) const;
    const ModelInfo *modelById(const QString &id) const;
    void touchNode(core::Node *node);
    void decodeResult(int nodeId, const QString &path);

    void onModelsFetched(const QVector<ModelInfo> &models);
    void onJobSubmitted(int nodeId, const QString &jobId, double estimatedCostUsd);
    void onSubmitRefused(int nodeId, SubmitRefusal refusal, const QString &detail);
    void onJobUpdated(const JobUpdate &update);
    void onStreamClosed(const QString &jobId);

    core::NodeGraph *m_graph = nullptr;
    GenerationClient *m_client = nullptr;
    bool m_serviceReady = false;
    QString m_unavailableReason;
    QVector<ModelInfo> m_models;

    QHash<QString, int> m_nodeByJob;
    QHash<int, QString> m_jobByNode;
};

} // namespace cutpilot::ipc
