#include "cutpilot/ipc/GenerationClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace cutpilot::ipc {

namespace {

JobState stateFromString(const QString &state)
{
    if (state == QStringLiteral("running"))
        return JobState::Running;
    if (state == QStringLiteral("done"))
        return JobState::Done;
    if (state == QStringLiteral("error"))
        return JobState::Error;
    if (state == QStringLiteral("canceled"))
        return JobState::Canceled;
    return JobState::Queued;
}

JobUpdate updateFromJson(const QJsonObject &object)
{
    JobUpdate update;
    update.jobId = object.value(QStringLiteral("job_id")).toString();
    update.state = stateFromString(object.value(QStringLiteral("state")).toString());
    update.progress = object.value(QStringLiteral("progress")).toDouble();
    update.message = object.value(QStringLiteral("message")).toString();
    update.resultPath = object.value(QStringLiteral("result_path")).toString();
    update.resultDigest = object.value(QStringLiteral("result_digest")).toString();
    update.costUsd = object.value(QStringLiteral("cost_usd")).toDouble(-1.0);
    update.width = object.value(QStringLiteral("width")).toInt();
    update.height = object.value(QStringLiteral("height")).toInt();
    return update;
}

} // namespace

GenerationClient::GenerationClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void GenerationClient::setEndpoint(quint16 port, const QByteArray &token)
{
    m_port = port;
    m_token = token;
}

QNetworkReply *GenerationClient::get(const QString &path)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
    request.setRawHeader("Authorization", "Bearer " + m_token);
    return m_network->get(request);
}

QNetworkReply *GenerationClient::post(const QString &path, const QByteArray &body)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
    request.setRawHeader("Authorization", "Bearer " + m_token);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    return m_network->post(request, body);
}

void GenerationClient::checkHealth()
{
    if (!ready()) {
        emit healthChecked(false, QString());
        return;
    }
    QNetworkReply *reply = get(QStringLiteral("/health"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit healthChecked(false, QString());
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        emit healthChecked(body.value(QStringLiteral("status")).toString()
                               == QStringLiteral("serving"),
                           body.value(QStringLiteral("version")).toString());
    });
}

void GenerationClient::fetchModels()
{
    if (!ready()) {
        emit modelsFailed(QStringLiteral("Generation service unavailable"));
        return;
    }
    QNetworkReply *reply = get(QStringLiteral("/models"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit modelsFailed(reply->errorString());
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        QVector<ModelInfo> models;
        const QJsonArray entries = body.value(QStringLiteral("models")).toArray();
        models.reserve(entries.size());
        for (const auto &entry : entries) {
            const QJsonObject object = entry.toObject();
            ModelInfo model;
            model.id = object.value(QStringLiteral("id")).toString();
            model.label = object.value(QStringLiteral("label")).toString();
            model.provider = object.value(QStringLiteral("provider")).toString();
            model.priceUsd = object.value(QStringLiteral("price_usd")).toDouble();
            model.needsKey = object.value(QStringLiteral("needs_key")).toBool();
            model.hasKey = object.value(QStringLiteral("has_key")).toBool();
            model.needsPrompt =
                object.value(QStringLiteral("needs_prompt")).toBool(true);
            model.needsInput =
                object.value(QStringLiteral("needs_input")).toBool(false);
            models.push_back(model);
        }
        emit modelsFetched(models);
    });
}

void GenerationClient::submitJob(int contextId, const GenerationRequest &request)
{
    if (!ready()) {
        emit submitRefused(contextId, SubmitRefusal::Unavailable,
                           QStringLiteral("Generation service unavailable"));
        return;
    }

    QJsonObject payload{
        { QStringLiteral("model"), request.modelId },
        { QStringLiteral("prompt"), request.prompt },
        { QStringLiteral("width"), request.width },
        { QStringLiteral("height"), request.height },
        { QStringLiteral("seed"), request.seed },
    };
    if (!request.inputPath.isEmpty())
        payload.insert(QStringLiteral("input_path"), request.inputPath);
    QNetworkReply *reply = post(QStringLiteral("/jobs"),
                                QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, contextId] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();

        if (status == 202) {
            emit jobSubmitted(
                contextId, body.value(QStringLiteral("job_id")).toString(),
                body.value(QStringLiteral("estimated_cost_usd")).toDouble(-1.0));
            return;
        }
        if (status == 409
            && body.value(QStringLiteral("error")).toString()
                == QStringLiteral("missing_key")) {
            emit submitRefused(contextId, SubmitRefusal::MissingKey,
                               body.value(QStringLiteral("provider")).toString());
            return;
        }
        if (status >= 400 && status < 500) {
            emit submitRefused(contextId, SubmitRefusal::Invalid,
                               body.value(QStringLiteral("error")).toString());
            return;
        }
        emit submitRefused(contextId, SubmitRefusal::Unavailable,
                           reply->error() != QNetworkReply::NoError
                               ? reply->errorString()
                               : QStringLiteral("Generation service unavailable"));
    });
}

void GenerationClient::subscribeJob(const QString &jobId)
{
    if (!ready())
        return;
    QNetworkReply *reply = get(QStringLiteral("/jobs/%1/events").arg(jobId));
    m_streamBuffers.insert(reply, QByteArray());

    connect(reply, &QNetworkReply::readyRead, this,
            [this, reply, jobId] { consumeStream(reply, jobId); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, jobId] {
        consumeStream(reply, jobId);
        const QByteArray leftover = m_streamBuffers.take(reply).trimmed();
        if (!leftover.isEmpty())
            qWarning("generation client: job %s stream ended mid-frame (%d bytes "
                     "dropped)",
                     qPrintable(jobId), int(leftover.size()));
        reply->deleteLater();
        emit streamClosed(jobId);
    });
}

void GenerationClient::consumeStream(QNetworkReply *reply, const QString &jobId)
{
    Q_UNUSED(jobId);
    QByteArray &buffer = m_streamBuffers[reply];
    buffer += reply->readAll();

    // Frames are separated by a blank line; each carries one data: payload.
    int boundary = -1;
    while ((boundary = buffer.indexOf("\n\n")) != -1) {
        const QByteArray frame = buffer.left(boundary);
        buffer.remove(0, boundary + 2);
        for (const QByteArray &line : frame.split('\n')) {
            if (!line.startsWith("data: "))
                continue;
            const QJsonObject object =
                QJsonDocument::fromJson(line.mid(6)).object();
            if (!object.isEmpty())
                emit jobUpdated(updateFromJson(object));
        }
    }
}

void GenerationClient::cancelJob(const QString &jobId)
{
    if (!ready())
        return;
    QNetworkReply *reply =
        post(QStringLiteral("/jobs/%1/cancel").arg(jobId), QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

} // namespace cutpilot::ipc
