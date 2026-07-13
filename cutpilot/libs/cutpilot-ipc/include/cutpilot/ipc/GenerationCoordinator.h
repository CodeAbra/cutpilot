#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVector>

#include "cutpilot/ipc/GenerationTypes.h"
#include "cutpilot/ipc/ResultCache.h"

namespace cutpilot::core {
class NodeGraph;
struct Node;
}

namespace cutpilot::ipc {

class GenerationClient;

// Drives generation for the node graph as a pipeline. A run — everything, the
// subgraph upstream of one node, or a forced re-run of one node — is ordered
// dependency-first, refuses cycles outright, and submits every node whose
// inputs are ready, so independent branches run concurrently. Results flow
// along the wires: an image-consuming node receives its upstream node's
// image file — a finished generation's result or a reference still's picked
// file — and each source's content digest keys the cache, so a node whose
// effective inputs and parameters are unchanged is served its previous
// result ("Reused") without a vendor call.
//
// Spending is governed twice: a run-wide cap pauses the whole run before a
// submission would cross it, and each cost-gate node holds just its
// downstream branch at the gate's own limit. Held work resumes after the
// limit is raised, or the run is aborted. Everything happens on this
// object's thread; the slow work stays in the service, so writing status is
// the only graph mutation and the UI thread never waits on a vendor.
//
// Prompt resolution order: the node wired into the generate node's text input
// supplies the prompt; without a wire the node's own prompt text is used; a
// prompt-needing model with an empty prompt refuses the run with guidance
// instead of submitting.
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

    // Re-pull the registry, e.g. after a key was added so per-provider key
    // presence is current.
    void refreshModels();

    // Start a run. Only one run is active at a time; starting another while
    // one is active is ignored. runNode is the node's run control: it
    // evaluates the subgraph upstream of the node, serving unchanged nodes
    // from cache. rerunNode forces the node itself to regenerate even when
    // its cached result would match.
    void runGraph();
    void runTo(int nodeId);
    void runNode(int nodeId);
    void rerunNode(int nodeId);

    void stopNode(int nodeId);

    // The run-wide spending cap in USD; zero means no cap. The active run
    // re-checks it on resume.
    void setRunCapUsd(double capUsd);
    double runCapUsd() const { return m_runCapUsd; }

    // Held work re-enters the run (a raised limit lets it through; an
    // unchanged one holds it again). abortRun cancels in-flight jobs and
    // settles everything pending back to idle.
    void resumeRun();
    void abortRun();

    bool runActive() const { return m_run.active; }
    RunSummary summary() const;

    // Drop run state that no longer matches reality: nodes claiming to run
    // without a live job fall back to idle, jobs whose node vanished are
    // canceled, and the active run forgets deleted nodes. Called after any
    // structural edit or history walk.
    void reconcile();

signals:
    // The node's textual/status content changed and its card should refresh.
    void nodeContentChanged(int nodeId);

    // The finished result decoded off the GUI thread and is ready to display.
    void nodeMediaReady(int nodeId, const QImage &image);

    void modelsReady();
    void addKeyNeeded(int nodeId, const QString &provider);

    // The run's live counts, spend, and pause state moved.
    void runSummaryChanged(const cutpilot::ipc::RunSummary &summary);

    // A run could not start at all (a dependency cycle, no service).
    void runRefused(const QString &reason);

private:
    // One pipeline run's bookkeeping. Nodes move pending → inFlight →
    // completed/reusedByCache/failed, or into held when a limit stops them.
    struct PipelineRun {
        bool active = false;
        bool capPaused = false;
        QString pauseReason;
        QVector<int> order;
        QSet<int> pending;
        QSet<int> inFlight;
        QSet<int> held;
        QSet<int> completed;
        QSet<int> reusedByCache;
        QSet<int> failed;
        QSet<int> forced;
        QHash<int, QString> signatures;
        QHash<int, double> committedUsd;
        double spentUsd = 0.0;
    };

    core::Node *generateNode(int nodeId);
    QString resolvePrompt(const core::Node &node) const;
    const ModelInfo *modelById(const QString &id) const;
    void touchNode(core::Node *node);
    void decodeResult(int nodeId, const QString &path);

    void startRun(const QVector<int> &subset, const QSet<int> &forced);
    void advanceRun();
    bool dependenciesSettled(int nodeId, const QHash<int, QSet<int>> &dependencies,
                             bool &upstreamFailed) const;
    QString nodeSignature(const core::Node &node, const ModelInfo &model);
    static QSize requestedSize(const core::Node &node);
    QString resolveInputPath(const core::Node &node) const;
    QVector<QString> inputDigests(const core::Node &node);

    // The image file an upstream node feeds downstream: a finished
    // generation's result, or a reference still's picked file.
    static QString imageSourcePath(const core::Node &source);

    // The content digest of one upstream image source, cached against the
    // file's size, modification time, and metadata-change time so an
    // unchanged reference is not re-hashed per advance. A freshly written
    // file is re-hashed unconditionally until its clocks can be trusted.
    QString sourceDigest(core::Node *source);
    bool applyCachedResult(core::Node *node, const QString &signature);
    void submitNode(core::Node *node, const ModelInfo &model,
                    const QString &signature);
    void settleDisownedSubmit(int nodeId);
    void failRunNode(int nodeId, const QString &message);
    void finishRunIfSettled();
    void updateGateReadouts();
    void refreshEstimates();
    void announceRun();

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

    // Nodes whose submission is on the wire but not yet acknowledged. A new
    // run on such a node is refused until the acknowledgement settles, so an
    // earlier request's acknowledgement can never be adopted as a later
    // run's job.
    QSet<int> m_awaitingSubmit;

    // The awaited submissions whose run was aborted. Their acknowledgements
    // cancel the job and settle the node back to idle instead of adopting
    // work nobody owns anymore.
    QSet<int> m_disownedSubmits;

    ResultCache m_cache;
    PipelineRun m_run;
    double m_runCapUsd = 0.0;

    // Reference-file digests keyed by path; the fingerprint (size + mtime +
    // metadata-change time) invalidates an entry when the file changes on
    // disk, and reconcile() drops entries no node references anymore.
    struct FileDigest {
        QString fingerprint;
        QString digest;
    };
    QHash<QString, FileDigest> m_fileDigests;
};

} // namespace cutpilot::ipc
