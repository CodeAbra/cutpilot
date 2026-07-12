#include "cutpilot/ipc/GenerationCoordinator.h"

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace cutpilot::ipc {

GenerationCoordinator::GenerationCoordinator(core::NodeGraph *graph,
                                             GenerationClient *client,
                                             QObject *parent)
    : QObject(parent)
    , m_graph(graph)
    , m_client(client)
{
    connect(client, &GenerationClient::modelsFetched, this,
            &GenerationCoordinator::onModelsFetched);
    connect(client, &GenerationClient::jobSubmitted, this,
            &GenerationCoordinator::onJobSubmitted);
    connect(client, &GenerationClient::submitRefused, this,
            &GenerationCoordinator::onSubmitRefused);
    connect(client, &GenerationClient::jobUpdated, this,
            &GenerationCoordinator::onJobUpdated);
    connect(client, &GenerationClient::streamClosed, this,
            &GenerationCoordinator::onStreamClosed);
}

void GenerationCoordinator::serviceBecameReady()
{
    m_serviceReady = true;
    m_unavailableReason.clear();
    m_client->fetchModels();
}

void GenerationCoordinator::refreshModels()
{
    if (m_serviceReady)
        m_client->fetchModels();
}

void GenerationCoordinator::serviceBecameUnavailable(const QString &reason)
{
    m_serviceReady = false;
    m_unavailableReason = reason;
}

core::Node *GenerationCoordinator::generateNode(int nodeId)
{
    core::Node *node = m_graph->nodeById(nodeId);
    return (node && node->kind == core::NodeKind::Generate) ? node : nullptr;
}

QString GenerationCoordinator::resolvePrompt(const core::Node &node) const
{
    for (int i = 0; i < node.ports.size(); ++i) {
        const core::Port &port = node.ports[i];
        if (!port.isInput || port.type != core::PortType::Text)
            continue;
        const int connectionId = m_graph->connectionAtInput(node.id, i);
        if (connectionId == -1)
            continue;
        const core::Connection *edge = m_graph->connectionById(connectionId);
        const core::Node *source = edge ? m_graph->nodeById(edge->fromNodeId) : nullptr;
        if (source && !source->promptText.trimmed().isEmpty())
            return source->promptText.trimmed();
    }
    return node.promptText.trimmed();
}

const ModelInfo *GenerationCoordinator::modelById(const QString &id) const
{
    for (const ModelInfo &model : m_models) {
        if (model.id == id)
            return &model;
    }
    return nullptr;
}

void GenerationCoordinator::touchNode(core::Node *node)
{
    node->bumpContent();
    emit nodeContentChanged(node->id);
}

void GenerationCoordinator::runNode(int nodeId)
{
    core::Node *node = generateNode(nodeId);
    if (!node)
        return;
    if (node->runState == core::RunState::Queued
        || node->runState == core::RunState::Running)
        return; // already in flight; stopping is the only valid action

    if (!m_serviceReady) {
        node->runState = core::RunState::Error;
        node->statusMessage = m_unavailableReason.isEmpty()
            ? QStringLiteral("Generation service unavailable")
            : m_unavailableReason;
        touchNode(node);
        return;
    }

    const QString prompt = resolvePrompt(*node);
    if (prompt.isEmpty()) {
        node->runState = core::RunState::Error;
        node->statusMessage = QStringLiteral("Add a prompt");
        touchNode(node);
        return;
    }

    QString modelId = node->modelId;
    if (modelId.isEmpty() && !m_models.isEmpty()) {
        // No explicit pick: the registry's first entry is the default.
        node->modelId = m_models.first().id;
        node->modelLabel = m_models.first().label;
        modelId = node->modelId;
    }
    if (modelId.isEmpty()) {
        node->runState = core::RunState::Error;
        node->statusMessage = QStringLiteral("Pick a model");
        touchNode(node);
        return;
    }

    GenerationRequest request;
    request.modelId = modelId;
    request.prompt = prompt;
    request.seed = nodeId; // stable per node, so a re-run reproduces its result

    node->runState = core::RunState::Queued;
    node->runProgress = 0.0;
    if (const ModelInfo *model = modelById(modelId)) {
        node->statusMessage =
            QStringLiteral("Queued · ~$%1").arg(model->priceUsd, 0, 'f', 3);
    } else {
        node->statusMessage = QStringLiteral("Queued");
    }
    touchNode(node);

    m_client->submitJob(nodeId, request);
}

void GenerationCoordinator::stopNode(int nodeId)
{
    const QString jobId = m_jobByNode.value(nodeId);
    if (jobId.isEmpty())
        return;
    if (core::Node *node = generateNode(nodeId)) {
        node->statusMessage = QStringLiteral("Stopping");
        touchNode(node);
    }
    m_client->cancelJob(jobId);
}

void GenerationCoordinator::reconcile()
{
    // Nodes claiming to run without a live job (restored by undo, or edited
    // while a submission was refused mid-flight) fall back to idle. Ids are
    // collected first: touching a node emits synchronously, and a slot that
    // edits the graph would invalidate an iterator held across the emit.
    QVector<int> orphaned;
    for (const core::Node &node : m_graph->nodes()) {
        if (node.kind != core::NodeKind::Generate)
            continue;
        const bool claimsLive = node.runState == core::RunState::Queued
            || node.runState == core::RunState::Running;
        if (claimsLive && !m_jobByNode.contains(node.id))
            orphaned.push_back(node.id);
    }
    for (int nodeId : orphaned) {
        core::Node *node = m_graph->nodeById(nodeId);
        if (!node)
            continue;
        node->runState = core::RunState::Idle;
        node->runProgress = 0.0;
        node->statusMessage.clear();
        touchNode(node);
    }

    // Jobs whose node vanished keep spending; cancel them and drop their
    // bookkeeping so a later stream close cannot touch another run.
    for (auto it = m_jobByNode.begin(); it != m_jobByNode.end();) {
        if (!m_graph->nodeById(it.key())) {
            m_client->cancelJob(it.value());
            m_nodeByJob.remove(it.value());
            it = m_jobByNode.erase(it);
        } else {
            ++it;
        }
    }
}

void GenerationCoordinator::onModelsFetched(const QVector<ModelInfo> &models)
{
    m_models = models;
    if (!m_models.isEmpty()) {
        // Ids first, then per-id touches: the synchronous emit inside
        // touchNode must never run against a live iterator over the graph.
        QVector<int> modelless;
        for (const core::Node &node : m_graph->nodes()) {
            if (node.kind == core::NodeKind::Generate && node.modelId.isEmpty())
                modelless.push_back(node.id);
        }
        for (int nodeId : modelless) {
            core::Node *node = m_graph->nodeById(nodeId);
            if (!node)
                continue;
            node->modelId = m_models.first().id;
            node->modelLabel = m_models.first().label;
            touchNode(node);
        }
    }
    emit modelsReady();
}

void GenerationCoordinator::onJobSubmitted(int nodeId, const QString &jobId,
                                           double estimatedCostUsd)
{
    Q_UNUSED(estimatedCostUsd);
    core::Node *node = generateNode(nodeId);
    if (!node) {
        m_client->cancelJob(jobId);
        return;
    }
    m_nodeByJob.insert(jobId, nodeId);
    m_jobByNode.insert(nodeId, jobId);
    m_client->subscribeJob(jobId);
}

void GenerationCoordinator::onSubmitRefused(int nodeId, SubmitRefusal refusal,
                                            const QString &detail)
{
    core::Node *node = generateNode(nodeId);
    if (!node)
        return;

    switch (refusal) {
    case SubmitRefusal::MissingKey:
        node->runState = core::RunState::NeedsKey;
        node->statusMessage = QStringLiteral("Add a key · %1").arg(detail);
        touchNode(node);
        emit addKeyNeeded(nodeId, detail);
        return;
    case SubmitRefusal::Invalid:
        node->runState = core::RunState::Error;
        node->statusMessage = QStringLiteral("Run refused: %1").arg(detail);
        break;
    case SubmitRefusal::Unavailable:
        node->runState = core::RunState::Error;
        node->statusMessage = detail;
        break;
    }
    touchNode(node);
}

void GenerationCoordinator::onJobUpdated(const JobUpdate &update)
{
    const int nodeId = m_nodeByJob.value(update.jobId, -1);
    if (nodeId == -1)
        return;
    core::Node *node = generateNode(nodeId);
    if (!node) {
        m_client->cancelJob(update.jobId);
        return;
    }

    switch (update.state) {
    case JobState::Queued:
        node->runState = core::RunState::Queued;
        break;
    case JobState::Running:
        node->runState = core::RunState::Running;
        node->runProgress = update.progress;
        node->statusMessage = QStringLiteral("Generating");
        break;
    case JobState::Done:
        node->runState = core::RunState::Done;
        node->runProgress = 1.0;
        node->statusMessage.clear();
        node->resultPath = update.resultPath;
        node->costUsd = update.costUsd;
        node->resultWidth = update.width;
        node->resultHeight = update.height;
        decodeResult(nodeId, update.resultPath);
        break;
    case JobState::Error:
        node->runState = core::RunState::Error;
        node->runProgress = 0.0;
        node->statusMessage = update.message.isEmpty()
            ? QStringLiteral("Generation failed")
            : update.message;
        break;
    case JobState::Canceled:
        // A stopped run keeps whatever result the node already had.
        node->runState = node->resultPath.isEmpty() ? core::RunState::Idle
                                                    : core::RunState::Done;
        node->runProgress = 0.0;
        node->statusMessage = QStringLiteral("Stopped");
        break;
    }
    touchNode(node);
}

void GenerationCoordinator::onStreamClosed(const QString &jobId)
{
    const auto mapping = m_nodeByJob.constFind(jobId);
    if (mapping == m_nodeByJob.constEnd())
        return;
    const int nodeId = mapping.value();
    m_nodeByJob.erase(mapping);

    // Only the node's current job may clear its bookkeeping: a stale
    // stream closing late must not clobber a newer run on the same node.
    if (m_jobByNode.value(nodeId) != jobId)
        return;
    m_jobByNode.remove(nodeId);

    // A stream that closed without reaching a terminal state means the
    // service went away mid-job.
    core::Node *node = generateNode(nodeId);
    if (node
        && (node->runState == core::RunState::Queued
            || node->runState == core::RunState::Running)) {
        node->runState = core::RunState::Error;
        node->statusMessage = QStringLiteral("Generation service unavailable");
        touchNode(node);
    }
}

void GenerationCoordinator::decodeResult(int nodeId, const QString &path)
{
    // Media decode stays off the GUI thread; the watcher delivers the image
    // back on this thread when it is ready to display.
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, nodeId] {
        watcher->deleteLater();
        const QImage image = watcher->result();
        if (!image.isNull() && m_graph->nodeById(nodeId))
            emit nodeMediaReady(nodeId, image);
    });
    watcher->setFuture(QtConcurrent::run([path] { return QImage(path); }));
}

} // namespace cutpilot::ipc
