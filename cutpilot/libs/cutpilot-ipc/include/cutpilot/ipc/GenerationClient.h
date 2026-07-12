#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QVector>

#include "cutpilot/ipc/GenerationTypes.h"

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace cutpilot::ipc {

// The typed async client for the generation service. Every call returns
// immediately and reports through a signal on this object's thread, so a
// caller on the GUI thread never blocks on the service. Job progress arrives
// as a server-sent event stream parsed incrementally as bytes land.
class GenerationClient : public QObject {
    Q_OBJECT

public:
    explicit GenerationClient(QObject *parent = nullptr);

    // Point the client at a running service. Until this is called the client
    // is not ready and submissions are refused as Unavailable.
    void setEndpoint(quint16 port, const QByteArray &token);
    bool ready() const { return m_port != 0; }

    void checkHealth();
    void fetchModels();

    // Submit a job on behalf of a caller-side context (the node id). The
    // context rides along so refusals and acceptances land back on the right
    // node without the caller tracking replies.
    void submitJob(int contextId, const GenerationRequest &request);

    // Open the job's event stream; updates arrive one jobUpdated at a time.
    void subscribeJob(const QString &jobId);

    void cancelJob(const QString &jobId);

signals:
    void healthChecked(bool serving, const QString &version);
    void modelsFetched(const QVector<ModelInfo> &models);
    void modelsFailed(const QString &error);
    void jobSubmitted(int contextId, const QString &jobId, double estimatedCostUsd);
    void submitRefused(int contextId, cutpilot::ipc::SubmitRefusal refusal,
                       const QString &detail);
    void jobUpdated(const cutpilot::ipc::JobUpdate &update);
    void streamClosed(const QString &jobId);

private:
    QNetworkReply *get(const QString &path);
    QNetworkReply *post(const QString &path, const QByteArray &body);
    void consumeStream(QNetworkReply *reply, const QString &jobId);

    QNetworkAccessManager *m_network = nullptr;
    quint16 m_port = 0;
    QByteArray m_token;

    // Per-stream receive buffers; frames are split on the blank line.
    QHash<QNetworkReply *, QByteArray> m_streamBuffers;
};

} // namespace cutpilot::ipc
