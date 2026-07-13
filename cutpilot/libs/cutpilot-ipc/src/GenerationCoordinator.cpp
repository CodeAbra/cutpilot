#include "cutpilot/ipc/GenerationCoordinator.h"

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/PipelineOrder.h"
#include "cutpilot/ipc/GenerationClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace cutpilot::ipc {

namespace {

QString money(double usd)
{
    return QStringLiteral("$%1").arg(usd, 0, 'f', 3);
}

// How old a file's last write must be before size and timestamps are trusted
// to detect change; a digest hashed before that settles is re-hashed on the
// next decision, so a rewrite landing inside one clock tick can never leave
// a stale digest behind.
constexpr qint64 kTrustFileClockAfterMs = 2000;

// Files up to this size hash inline within a frame's budget; anything larger
// hashes off the GUI thread so a run decision never stalls on a big file.
constexpr qint64 kInlineHashMaxBytes = qint64(1) * 1024 * 1024;

// SHA-256 of a file's contents, or an empty string when unreadable.
QString hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

// Size, modification time, and metadata-change time together fingerprint the
// file: a rewrite that forges size and mtime still bumps the metadata clock,
// which userland cannot set back.
QString fileFingerprint(const QFileInfo &info)
{
    return QStringLiteral("%1|%2|%3")
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(info.metadataChangeTime().toMSecsSinceEpoch());
}

} // namespace

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

QString GenerationCoordinator::imageSourcePath(const core::Node &source)
{
    if (!source.resultPath.isEmpty())
        return source.resultPath;
    // A reference still feeds its picked file downstream exactly as a
    // finished generation feeds its result.
    if (source.kind == core::NodeKind::Still && !source.mediaPath.isEmpty())
        return source.mediaPath;
    return QString();
}

QString GenerationCoordinator::sourceDigest(core::Node *source, bool &pending)
{
    if (!source->resultPath.isEmpty()) {
        if (source->resultDigest.isEmpty()) {
            bool digestPending = false;
            const QString digest =
                fileDigestFor(source->resultPath, digestPending);
            if (digestPending) {
                pending = true;
                return QString();
            }
            source->resultDigest = digest;
        }
        return source->resultDigest;
    }

    const QString path = imageSourcePath(*source);
    if (path.isEmpty())
        return QString();
    return fileDigestFor(path, pending);
}

QString GenerationCoordinator::fileDigestFor(const QString &path, bool &pending)
{
    const QFileInfo info(path);
    const QString fingerprint = fileFingerprint(info);
    FileDigest &entry = m_fileDigests[path];

    // An off-thread hash that just landed answers exactly one decision, even
    // while the file's clocks are still too young to be trusted — the same
    // trust an inline hash computed this instant would get. "Just" is load
    // bearing: a delivery nobody consumed promptly (an aborted run, a node
    // that vanished mid-hash) ages out and must re-derive trust instead.
    if (entry.freshDelivery && entry.fingerprint == fingerprint
        && QDateTime::currentMSecsSinceEpoch() - entry.hashedAtMs
            < kTrustFileClockAfterMs) {
        entry.freshDelivery = false;
        return entry.digest;
    }

    // A cached digest stands only if the file is unchanged and the hash was
    // computed after the write's clock tick could no longer be re-entered;
    // a hash taken inside that window may predate a same-tick rewrite.
    const bool clocksSettled = entry.hashedAtMs
        >= info.lastModified().toMSecsSinceEpoch() + kTrustFileClockAfterMs;
    if (!entry.digest.isEmpty() && entry.fingerprint == fingerprint
        && clocksSettled)
        return entry.digest;

    if (info.size() <= kInlineHashMaxBytes) {
        entry.fingerprint = fingerprint;
        entry.digest = hashFile(path);
        entry.hashedAtMs = QDateTime::currentMSecsSinceEpoch();
        entry.freshDelivery = false;
        return entry.digest;
    }

    if (!m_hashesInFlight.contains(path))
        hashFileOffThread(path);
    pending = true;
    return QString();
}

void GenerationCoordinator::hashFileOffThread(const QString &path)
{
    m_hashesInFlight.insert(path);
    auto *watcher = new QFutureWatcher<FileDigest>(this);
    connect(watcher, &QFutureWatcher<FileDigest>::finished, this,
            [this, watcher, path] {
                watcher->deleteLater();
                m_hashesInFlight.remove(path);
                FileDigest entry = watcher->result();
                entry.freshDelivery = true;
                m_fileDigests.insert(path, entry);
                if (m_run.active)
                    advanceRun();
            });
    watcher->setFuture(QtConcurrent::run([path] {
        FileDigest entry;
        // Fingerprint before reading, like the inline path: a rewrite during
        // the hash lands a fingerprint mismatch on the next decision.
        entry.fingerprint = fileFingerprint(QFileInfo(path));
        entry.digest = hashFile(path);
        entry.hashedAtMs = QDateTime::currentMSecsSinceEpoch();
        return entry;
    }));
}

QString GenerationCoordinator::resolveInputPath(const core::Node &node) const
{
    for (int i = 0; i < node.ports.size(); ++i) {
        const core::Port &port = node.ports[i];
        if (!port.isInput || port.type != core::PortType::Image)
            continue;
        const int connectionId = m_graph->connectionAtInput(node.id, i);
        if (connectionId == -1)
            continue;
        const core::Connection *edge = m_graph->connectionById(connectionId);
        const core::Node *source = edge ? m_graph->nodeById(edge->fromNodeId) : nullptr;
        if (!source)
            continue;
        const QString path = imageSourcePath(*source);
        if (!path.isEmpty())
            return path;
    }
    return QString();
}

QVector<QString> GenerationCoordinator::inputDigests(const core::Node &node,
                                                     bool &pending)
{
    QVector<QString> digests;
    for (int i = 0; i < node.ports.size(); ++i) {
        const core::Port &port = node.ports[i];
        // Text feeds the resolved prompt, which the signature already
        // carries; control wires carry policy, not data.
        if (!port.isInput || port.type == core::PortType::Text
            || port.type == core::PortType::Control)
            continue;
        const int connectionId = m_graph->connectionAtInput(node.id, i);
        if (connectionId == -1)
            continue;
        const core::Connection *edge = m_graph->connectionById(connectionId);
        core::Node *source = edge ? m_graph->nodeById(edge->fromNodeId) : nullptr;
        if (!source)
            continue;
        const QString digest = sourceDigest(source, pending);
        if (digest.isEmpty())
            continue;
        digests.push_back(QStringLiteral("%1=%2").arg(i).arg(digest));
    }
    return digests;
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

void GenerationCoordinator::runGraph()
{
    QVector<int> subset;
    for (const core::Node &node : m_graph->nodes()) {
        if (core::isEvaluatable(node))
            subset.push_back(node.id);
    }
    startRun(subset, {});
}

void GenerationCoordinator::runTo(int nodeId)
{
    core::Node *node = generateNode(nodeId);
    if (!node)
        return;

    if (!m_serviceReady) {
        node->runState = core::RunState::Error;
        node->statusMessage = m_unavailableReason.isEmpty()
            ? QStringLiteral("Generation service unavailable")
            : m_unavailableReason;
        touchNode(node);
        return;
    }

    const QSet<int> upstream = core::upstreamClosure(*m_graph, nodeId);
    QVector<int> subset;
    for (const core::Node &candidate : m_graph->nodes()) {
        if (core::isEvaluatable(candidate) && upstream.contains(candidate.id))
            subset.push_back(candidate.id);
    }
    startRun(subset, {});
}

void GenerationCoordinator::runNode(int nodeId)
{
    runTo(nodeId);
}

void GenerationCoordinator::rerunNode(int nodeId)
{
    core::Node *node = generateNode(nodeId);
    if (!node)
        return;
    if (!m_serviceReady) {
        node->runState = core::RunState::Error;
        node->statusMessage = m_unavailableReason.isEmpty()
            ? QStringLiteral("Generation service unavailable")
            : m_unavailableReason;
        touchNode(node);
        return;
    }

    const QSet<int> upstream = core::upstreamClosure(*m_graph, nodeId);
    QVector<int> subset;
    for (const core::Node &candidate : m_graph->nodes()) {
        if (core::isEvaluatable(candidate) && upstream.contains(candidate.id))
            subset.push_back(candidate.id);
    }
    startRun(subset, { nodeId });
}

void GenerationCoordinator::startRun(const QVector<int> &subset,
                                     const QSet<int> &forced)
{
    if (m_run.active) {
        emit runRefused(QStringLiteral("A run is already active"));
        return;
    }
    if (subset.isEmpty())
        return;
    if (!m_serviceReady) {
        emit runRefused(m_unavailableReason.isEmpty()
                            ? QStringLiteral("Generation service unavailable")
                            : m_unavailableReason);
        return;
    }
    for (int nodeId : subset) {
        if (m_jobByNode.contains(nodeId) || m_awaitingSubmit.contains(nodeId)) {
            emit runRefused(QStringLiteral("Still stopping the previous run"));
            return;
        }
    }

    const core::EvaluationPlan plan = core::evaluationOrder(*m_graph, subset);
    if (plan.hasCycle) {
        emit runRefused(QStringLiteral(
            "The graph loops back on itself; disconnect the cycle to run"));
        return;
    }

    m_run = PipelineRun();
    m_run.active = true;
    m_run.order = plan.order;
    m_run.pending = QSet<int>(plan.order.cbegin(), plan.order.cend());
    m_run.forced = forced;

    // Each run starts a fresh spend ledger on every gate.
    QVector<int> gates;
    for (const core::Node &node : m_graph->nodes()) {
        if (node.kind == core::NodeKind::CostGate)
            gates.push_back(node.id);
    }
    for (int gateId : gates) {
        core::Node *gate = m_graph->nodeById(gateId);
        if (!gate)
            continue;
        gate->gateSpentUsd = 0.0;
        gate->statusMessage.clear();
        touchNode(gate);
    }

    advanceRun();
}

bool GenerationCoordinator::dependenciesSettled(
    int nodeId, const QHash<int, QSet<int>> &dependencies, bool &upstreamFailed) const
{
    upstreamFailed = false;
    for (int dep : dependencies.value(nodeId)) {
        if (m_run.failed.contains(dep)) {
            upstreamFailed = true;
            return false;
        }
        if (m_run.completed.contains(dep) || m_run.reusedByCache.contains(dep))
            continue;
        if (m_run.pending.contains(dep) || m_run.inFlight.contains(dep)
            || m_run.held.contains(dep))
            return false;
        // Outside the run entirely: its existing result stands in.
        const core::Node *node = m_graph->nodeById(dep);
        if (!node || node->runState != core::RunState::Done
            || node->resultPath.isEmpty())
            return false;
    }
    return true;
}

QString GenerationCoordinator::nodeSignature(const core::Node &node,
                                             const ModelInfo &model,
                                             bool &pending)
{
    const QSize size = requestedSize(node);
    return ResultCache::signature(model.id, resolvePrompt(node), size.width(),
                                  size.height(), node.id,
                                  inputDigests(node, pending));
}

// The node's requested output size, falling back to the request defaults when
// unset. One resolver feeds both the submission and the cache signature, so a
// format change can never be served a stale cached result.
QSize GenerationCoordinator::requestedSize(const core::Node &node)
{
    const GenerationRequest defaults;
    if (node.outputWidth > 0 && node.outputHeight > 0)
        return QSize(node.outputWidth, node.outputHeight);
    return QSize(defaults.width, defaults.height);
}

bool GenerationCoordinator::applyCachedResult(core::Node *node,
                                              const QString &signature)
{
    const std::optional<ResultCache::Entry> entry = m_cache.lookup(signature);
    if (!entry)
        return false;

    node->runState = core::RunState::Done;
    node->runProgress = 1.0;
    node->statusMessage = QStringLiteral("Reused");
    node->resultPath = entry->resultPath;
    node->resultDigest = entry->resultDigest;
    node->costUsd = entry->costUsd;
    node->resultWidth = entry->width;
    node->resultHeight = entry->height;
    touchNode(node);
    decodeResult(node->id, entry->resultPath);
    return true;
}

void GenerationCoordinator::submitNode(core::Node *node, const ModelInfo &model,
                                       const QString &signature)
{
    GenerationRequest request;
    request.modelId = model.id;
    request.prompt = resolvePrompt(*node);
    request.inputPath = resolveInputPath(*node);
    const QSize size = requestedSize(*node);
    request.width = size.width();
    request.height = size.height();
    request.seed = node->id; // stable per node, so a re-run reproduces its result

    m_run.signatures.insert(node->id, signature);
    m_run.committedUsd.insert(node->id, model.priceUsd);
    m_run.pending.remove(node->id);
    m_run.inFlight.insert(node->id);
    m_awaitingSubmit.insert(node->id);

    node->runState = core::RunState::Queued;
    node->runProgress = 0.0;
    node->statusMessage = QStringLiteral("Queued · ~%1").arg(money(model.priceUsd));
    touchNode(node);

    m_client->submitJob(node->id, request);
}

void GenerationCoordinator::settleDisownedSubmit(int nodeId)
{
    core::Node *node = generateNode(nodeId);
    if (!node)
        return;
    if (node->runState != core::RunState::Queued
        && node->runState != core::RunState::Running)
        return;
    node->runState = core::RunState::Idle;
    node->runProgress = 0.0;
    node->statusMessage.clear();
    touchNode(node);
}

void GenerationCoordinator::failRunNode(int nodeId, const QString &message)
{
    m_run.pending.remove(nodeId);
    m_run.inFlight.remove(nodeId);
    m_run.held.remove(nodeId);
    m_run.committedUsd.remove(nodeId);
    m_run.failed.insert(nodeId);

    if (core::Node *node = generateNode(nodeId)) {
        node->runState = core::RunState::Error;
        node->statusMessage = message;
        touchNode(node);
    }
}

void GenerationCoordinator::advanceRun()
{
    if (!m_run.active)
        return;

    const QHash<int, QSet<int>> dependencies = core::generationDependencies(*m_graph);

    double committedTotal = 0.0;
    for (double estimate : m_run.committedUsd)
        committedTotal += estimate;

    // Governed sets and per-gate spend, refreshed as the pass progresses.
    QVector<int> gateIds;
    for (const core::Node &node : m_graph->nodes()) {
        if (node.kind == core::NodeKind::CostGate)
            gateIds.push_back(node.id);
    }
    QHash<int, QSet<int>> governed;
    for (int gateId : gateIds) {
        QSet<int> branch = core::downstreamClosure(*m_graph, gateId);
        branch.remove(gateId);
        governed.insert(gateId, branch);
    }
    const auto gateSpend = [this, &governed](int gateId) {
        double spend = 0.0;
        const QSet<int> &branch = governed.value(gateId);
        for (int id : m_run.completed) {
            const core::Node *node = m_graph->nodeById(id);
            if (node && branch.contains(id) && node->costUsd > 0.0)
                spend += node->costUsd;
        }
        for (auto it = m_run.committedUsd.cbegin(); it != m_run.committedUsd.cend();
             ++it) {
            if (branch.contains(it.key()))
                spend += it.value();
        }
        return spend;
    };

    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (int nodeId : m_run.order) {
            if (!m_run.pending.contains(nodeId))
                continue;

            core::Node *node = generateNode(nodeId);
            if (!node) {
                // The node vanished mid-run; forget it entirely.
                m_run.pending.remove(nodeId);
                m_run.order.removeAll(nodeId);
                progressed = true;
                break; // the order list changed under the loop
            }

            bool upstreamFailed = false;
            if (!dependenciesSettled(nodeId, dependencies, upstreamFailed)) {
                if (upstreamFailed) {
                    m_run.pending.remove(nodeId);
                    m_run.failed.insert(nodeId);
                    node->runState = core::RunState::Idle;
                    node->runProgress = 0.0;
                    node->statusMessage = QStringLiteral("Skipped · upstream failed");
                    touchNode(node);
                    progressed = true;
                }
                continue;
            }

            const ModelInfo *model = modelById(node->modelId);
            if (!model) {
                failRunNode(nodeId, node->modelId.isEmpty()
                                ? QStringLiteral("Pick a model")
                                : QStringLiteral("Unknown model %1").arg(node->modelId));
                progressed = true;
                continue;
            }
            if (model->needsPrompt && resolvePrompt(*node).isEmpty()) {
                failRunNode(nodeId, QStringLiteral("Add a prompt"));
                progressed = true;
                continue;
            }
            if (model->needsInput) {
                const QString input = resolveInputPath(*node);
                if (input.isEmpty()) {
                    failRunNode(nodeId,
                                QStringLiteral("Connect an image input"));
                    progressed = true;
                    continue;
                }
                // A wired file can vanish outside the app; refuse locally
                // with guidance instead of submitting a job that names a
                // file nobody can read.
                if (!QFileInfo::exists(input)) {
                    failRunNode(nodeId,
                                QStringLiteral("Reference file missing"));
                    progressed = true;
                    continue;
                }
            }

            // A digest still hashing off-thread leaves the node pending
            // untouched; its arrival re-advances the run.
            bool digestsPending = false;
            const QString signature =
                nodeSignature(*node, *model, digestsPending);
            if (digestsPending)
                continue;

            if (!m_run.forced.contains(nodeId) && applyCachedResult(node, signature)) {
                m_run.pending.remove(nodeId);
                m_run.reusedByCache.insert(nodeId);
                progressed = true;
                continue;
            }

            // Once the run cap pauses the run nothing more is submitted;
            // in-flight work keeps streaming and cache hits above are still
            // served for free. Every other ready node is blocked by the same
            // pause, so it is held too — not left silently pending — and the
            // held count reflects everything the cap is actually stopping.
            if (m_run.capPaused) {
                m_run.pending.remove(nodeId);
                m_run.held.insert(nodeId);
                node->runState = core::RunState::Held;
                node->statusMessage =
                    QStringLiteral("Held · run cap %1").arg(money(m_runCapUsd));
                touchNode(node);
                progressed = true;
                continue;
            }

            if (m_runCapUsd > 0.0
                && m_run.spentUsd + committedTotal + model->priceUsd > m_runCapUsd) {
                m_run.pending.remove(nodeId);
                m_run.held.insert(nodeId);
                m_run.capPaused = true;
                m_run.pauseReason =
                    QStringLiteral("Run cap %1 reached").arg(money(m_runCapUsd));
                node->runState = core::RunState::Held;
                node->statusMessage =
                    QStringLiteral("Held · run cap %1").arg(money(m_runCapUsd));
                touchNode(node);
                progressed = true;
                continue;
            }

            int blockingGate = -1;
            for (int gateId : gateIds) {
                const core::Node *gate = m_graph->nodeById(gateId);
                if (!gate || !governed.value(gateId).contains(nodeId))
                    continue;
                if (gateSpend(gateId) + model->priceUsd > gate->gateLimitUsd) {
                    blockingGate = gateId;
                    break;
                }
            }
            if (blockingGate != -1) {
                const core::Node *gate = m_graph->nodeById(blockingGate);
                m_run.pending.remove(nodeId);
                m_run.held.insert(nodeId);
                if (m_run.pauseReason.isEmpty())
                    m_run.pauseReason = QStringLiteral("Cost gate at %1 reached")
                                            .arg(money(gate->gateLimitUsd));
                node->runState = core::RunState::Held;
                node->statusMessage = QStringLiteral("Held · cost gate at %1")
                                          .arg(money(gate->gateLimitUsd));
                touchNode(node);
                progressed = true;
                continue;
            }

            submitNode(node, *model, signature);
            committedTotal += model->priceUsd;
            progressed = true;
        }
    }

    updateGateReadouts();
    finishRunIfSettled();
    announceRun();
}

void GenerationCoordinator::updateGateReadouts()
{
    QVector<int> gateIds;
    for (const core::Node &node : m_graph->nodes()) {
        if (node.kind == core::NodeKind::CostGate)
            gateIds.push_back(node.id);
    }
    for (int gateId : gateIds) {
        core::Node *gate = m_graph->nodeById(gateId);
        if (!gate)
            continue;
        QSet<int> branch = core::downstreamClosure(*m_graph, gateId);
        branch.remove(gateId);

        double spend = 0.0;
        bool holding = false;
        for (int id : branch) {
            const core::Node *node = m_graph->nodeById(id);
            if (!node)
                continue;
            if (m_run.completed.contains(id) && node->costUsd > 0.0)
                spend += node->costUsd;
            if (m_run.committedUsd.contains(id))
                spend += m_run.committedUsd.value(id);
            if (m_run.held.contains(id))
                holding = true;
        }

        const QString message = holding
            ? QStringLiteral("Holding · %1 of %2")
                  .arg(money(spend), money(gate->gateLimitUsd))
            : QString();
        if (gate->gateSpentUsd != spend || gate->statusMessage != message) {
            gate->gateSpentUsd = spend;
            gate->statusMessage = message;
            touchNode(gate);
        }
    }
}

void GenerationCoordinator::finishRunIfSettled()
{
    if (!m_run.active)
        return;
    if (m_run.pending.isEmpty() && m_run.inFlight.isEmpty() && m_run.held.isEmpty()) {
        m_run.active = false;
        m_run.capPaused = false;
        m_run.pauseReason.clear();
    }
}

RunSummary GenerationCoordinator::summary() const
{
    RunSummary summary;
    summary.active = m_run.active;
    summary.paused = m_run.capPaused || !m_run.held.isEmpty();
    summary.pauseReason = m_run.pauseReason;
    summary.total = m_run.order.size();
    summary.fresh = m_run.completed.size();
    summary.reused = m_run.reusedByCache.size();
    summary.running = m_run.inFlight.size();
    summary.held = m_run.held.size();
    summary.failed = m_run.failed.size();
    summary.spentUsd = m_run.spentUsd;
    for (double estimate : m_run.committedUsd)
        summary.committedUsd += estimate;
    summary.capUsd = m_runCapUsd;
    return summary;
}

void GenerationCoordinator::announceRun()
{
    emit runSummaryChanged(summary());
}

void GenerationCoordinator::setRunCapUsd(double capUsd)
{
    m_runCapUsd = qMax(0.0, capUsd);
    announceRun();
}

void GenerationCoordinator::resumeRun()
{
    if (!m_run.active)
        return;
    m_run.capPaused = false;
    m_run.pauseReason.clear();

    const QSet<int> held = m_run.held;
    m_run.held.clear();
    for (int nodeId : held) {
        m_run.pending.insert(nodeId);
        if (core::Node *node = generateNode(nodeId)) {
            node->runState = core::RunState::Idle;
            node->statusMessage.clear();
            touchNode(node);
        }
    }
    advanceRun();
}

void GenerationCoordinator::abortRun()
{
    if (!m_run.active)
        return;

    // Deactivate first: the cancellations below stream terminal states back,
    // and those must be plain node updates, not run bookkeeping. Submissions
    // still on the wire are disowned, not forgotten: their acknowledgements
    // cancel the job and settle the node, and until they land a new run on
    // those nodes is refused rather than raced.
    m_run.active = false;
    const QSet<int> inFlight = m_run.inFlight;
    const QSet<int> parked = m_run.pending + m_run.held;
    m_run = PipelineRun();
    m_disownedSubmits.unite(m_awaitingSubmit);

    for (int nodeId : inFlight) {
        const QString jobId = m_jobByNode.value(nodeId);
        if (!jobId.isEmpty())
            m_client->cancelJob(jobId);
    }
    for (int nodeId : parked) {
        if (core::Node *node = generateNode(nodeId)) {
            node->runState = core::RunState::Idle;
            node->runProgress = 0.0;
            node->statusMessage.clear();
            touchNode(node);
        }
    }
    announceRun();
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
    // The active run forgets nodes that no longer exist, then re-advances:
    // a deletion can unblock a branch or settle the whole run.
    if (m_run.active) {
        const auto pruneSet = [this](QSet<int> &set) {
            for (auto it = set.begin(); it != set.end();) {
                if (!m_graph->nodeById(*it))
                    it = set.erase(it);
                else
                    ++it;
            }
        };
        pruneSet(m_run.pending);
        pruneSet(m_run.inFlight);
        pruneSet(m_run.held);
        pruneSet(m_run.completed);
        pruneSet(m_run.reusedByCache);
        pruneSet(m_run.failed);
        for (auto it = m_run.committedUsd.begin(); it != m_run.committedUsd.end();) {
            if (!m_graph->nodeById(it.key()))
                it = m_run.committedUsd.erase(it);
            else
                ++it;
        }
        for (auto it = m_run.order.begin(); it != m_run.order.end();) {
            if (!m_graph->nodeById(*it))
                it = m_run.order.erase(it);
            else
                ++it;
        }
    }

    // Nodes claiming live or held run state without the bookkeeping to back
    // it (restored by undo, or edited mid-refusal) fall back to idle. Ids are
    // collected first: touching a node emits synchronously, and a slot that
    // edits the graph would invalidate an iterator held across the emit.
    QVector<int> orphaned;
    for (const core::Node &node : m_graph->nodes()) {
        if (node.kind != core::NodeKind::Generate)
            continue;
        const bool claimsLive = node.runState == core::RunState::Queued
            || node.runState == core::RunState::Running;
        const bool claimsHeld = node.runState == core::RunState::Held;
        if (claimsLive && !m_jobByNode.contains(node.id)
            && !m_awaitingSubmit.contains(node.id))
            orphaned.push_back(node.id);
        else if (claimsHeld && !(m_run.active && m_run.held.contains(node.id)))
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
    for (auto it = m_awaitingSubmit.begin(); it != m_awaitingSubmit.end();) {
        if (!m_graph->nodeById(*it))
            it = m_awaitingSubmit.erase(it);
        else
            ++it;
    }
    for (auto it = m_disownedSubmits.begin(); it != m_disownedSubmits.end();) {
        if (!m_graph->nodeById(*it))
            it = m_disownedSubmits.erase(it);
        else
            ++it;
    }

    // The digest cache only saves re-hashing for files the board still
    // references; entries whose path no longer backs any node are dropped.
    QSet<QString> livePaths;
    for (const core::Node &node : m_graph->nodes()) {
        const QString source = imageSourcePath(node);
        if (!source.isEmpty())
            livePaths.insert(source);
    }
    for (auto it = m_fileDigests.begin(); it != m_fileDigests.end();) {
        if (!livePaths.contains(it.key()))
            it = m_fileDigests.erase(it);
        else
            ++it;
    }

    refreshEstimates();

    if (m_run.active)
        advanceRun();
}

void GenerationCoordinator::refreshEstimates()
{
    QVector<int> generateIds;
    for (const core::Node &node : m_graph->nodes()) {
        if (node.kind == core::NodeKind::Generate)
            generateIds.push_back(node.id);
    }
    for (int nodeId : generateIds) {
        core::Node *node = m_graph->nodeById(nodeId);
        if (!node)
            continue;
        const ModelInfo *model = modelById(node->modelId);
        const double estimate = model ? model->priceUsd : -1.0;
        if (node->estimatedCostUsd != estimate) {
            node->estimatedCostUsd = estimate;
            touchNode(node);
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
    refreshEstimates();
    emit modelsReady();
}

void GenerationCoordinator::onJobSubmitted(int nodeId, const QString &jobId,
                                           double estimatedCostUsd)
{
    Q_UNUSED(estimatedCostUsd);
    if (!m_awaitingSubmit.remove(nodeId)) {
        m_client->cancelJob(jobId);
        return;
    }
    if (m_disownedSubmits.remove(nodeId)) {
        m_client->cancelJob(jobId);
        settleDisownedSubmit(nodeId);
        return;
    }
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
    if (!m_awaitingSubmit.remove(nodeId))
        return;
    if (m_disownedSubmits.remove(nodeId)) {
        settleDisownedSubmit(nodeId);
        return;
    }
    const bool inRun = m_run.active && m_run.inFlight.contains(nodeId);
    if (inRun) {
        m_run.inFlight.remove(nodeId);
        m_run.committedUsd.remove(nodeId);
        m_run.failed.insert(nodeId);
    }

    core::Node *node = generateNode(nodeId);
    if (node) {
        switch (refusal) {
        case SubmitRefusal::MissingKey:
            node->runState = core::RunState::NeedsKey;
            node->statusMessage = QStringLiteral("Add a key · %1").arg(detail);
            touchNode(node);
            emit addKeyNeeded(nodeId, detail);
            break;
        case SubmitRefusal::Invalid:
            node->runState = core::RunState::Error;
            node->statusMessage = QStringLiteral("Run refused: %1").arg(detail);
            touchNode(node);
            break;
        case SubmitRefusal::Unavailable:
            node->runState = core::RunState::Error;
            node->statusMessage = detail;
            touchNode(node);
            break;
        }
    }

    if (inRun)
        advanceRun();
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

    const bool inRun = m_run.active && m_run.inFlight.contains(nodeId);
    bool settled = false;

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
        node->resultDigest = update.resultDigest;
        node->costUsd = update.costUsd;
        node->resultWidth = update.width;
        node->resultHeight = update.height;
        decodeResult(nodeId, update.resultPath);
        settled = true;
        if (inRun) {
            m_run.inFlight.remove(nodeId);
            m_run.committedUsd.remove(nodeId);
            m_run.completed.insert(nodeId);
            if (update.costUsd > 0.0)
                m_run.spentUsd += update.costUsd;
            const QString signature = m_run.signatures.value(nodeId);
            if (!signature.isEmpty()) {
                ResultCache::Entry entry;
                entry.resultPath = update.resultPath;
                entry.resultDigest = update.resultDigest;
                entry.costUsd = update.costUsd;
                entry.width = update.width;
                entry.height = update.height;
                m_cache.store(signature, entry);
            }
        }
        break;
    case JobState::Error:
        node->runState = core::RunState::Error;
        node->runProgress = 0.0;
        node->statusMessage = update.message.isEmpty()
            ? QStringLiteral("Generation failed")
            : update.message;
        settled = true;
        if (inRun) {
            m_run.inFlight.remove(nodeId);
            m_run.committedUsd.remove(nodeId);
            m_run.failed.insert(nodeId);
        }
        break;
    case JobState::Canceled:
        // A stopped run keeps whatever result the node already had.
        node->runState = node->resultPath.isEmpty() ? core::RunState::Idle
                                                    : core::RunState::Done;
        node->runProgress = 0.0;
        node->statusMessage = QStringLiteral("Stopped");
        settled = true;
        if (inRun) {
            m_run.inFlight.remove(nodeId);
            m_run.committedUsd.remove(nodeId);
            m_run.failed.insert(nodeId);
        }
        break;
    }
    touchNode(node);

    if (inRun && settled)
        advanceRun();
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
    if (m_run.active && m_run.inFlight.contains(nodeId)) {
        m_run.inFlight.remove(nodeId);
        m_run.committedUsd.remove(nodeId);
        m_run.failed.insert(nodeId);
        advanceRun();
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
