#include "cutpilot/ipc/ConvertClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace cutpilot::ipc {

ConvertClient::ConvertClient(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void ConvertClient::setEndpoint(quint16 port, const QByteArray &token)
{
    m_port = port;
    m_token = token;
}

QNetworkReply *ConvertClient::post(const QString &path, const QJsonObject &body)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)));
    request.setRawHeader("Authorization", "Bearer " + m_token);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    return m_network->post(request, QJsonDocument(body).toJson(
                                        QJsonDocument::Compact));
}

QString ConvertClient::replyError(QNetworkReply *reply, const QJsonObject &body)
{
    const QString detail = body.value(QStringLiteral("detail")).toString();
    if (!detail.isEmpty())
        return detail;
    const QString code = body.value(QStringLiteral("error")).toString();
    if (!code.isEmpty())
        return code;
    return reply->errorString();
}

void ConvertClient::exportTimeline(const QString &format,
                                   const QJsonObject &payload)
{
    if (!ready()) {
        emit timelineExportFailed(format,
                                  QStringLiteral("Convert service unavailable"));
        return;
    }
    QNetworkReply *reply =
        post(QStringLiteral("/timeline/%1").arg(format), payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, format] {
        reply->deleteLater();
        const QJsonObject body =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit timelineExportFailed(format, replyError(reply, body));
            return;
        }
        emit timelineExported(format,
                              body.value(QStringLiteral("path")).toString());
    });
}

void ConvertClient::importComfyWorkflow(const QJsonObject &workflow)
{
    if (!ready()) {
        emit comfyImportFailed(QStringLiteral("Convert service unavailable"));
        return;
    }
    QNetworkReply *reply =
        post(QStringLiteral("/comfyui/import"),
             QJsonObject{ { QStringLiteral("workflow"), workflow } });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject body =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit comfyImportFailed(replyError(reply, body));
            return;
        }
        emit comfyImported(body);
    });
}

void ConvertClient::exportComfyWorkflow(const QJsonArray &nodes,
                                        const QJsonArray &connections)
{
    if (!ready()) {
        emit comfyExportFailed(QStringLiteral("Convert service unavailable"));
        return;
    }
    QNetworkReply *reply =
        post(QStringLiteral("/comfyui/export"),
             QJsonObject{ { QStringLiteral("nodes"), nodes },
                          { QStringLiteral("connections"), connections } });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject body =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit comfyExportFailed(replyError(reply, body));
            return;
        }
        emit comfyExported(
            body.value(QStringLiteral("workflow")).toObject());
    });
}

void ConvertClient::importIntoResolve(const QString &path)
{
    if (!ready()) {
        emit resolveImportFinished(
            false, QStringLiteral("unavailable"),
            QStringLiteral("Convert service unavailable"));
        return;
    }
    QNetworkReply *reply =
        post(QStringLiteral("/resolve/import"),
             QJsonObject{ { QStringLiteral("path"), path } });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonObject body =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            emit resolveImportFinished(false, QStringLiteral("request_failed"),
                                       replyError(reply, body));
            return;
        }
        emit resolveImportFinished(
            body.value(QStringLiteral("ok")).toBool(),
            body.value(QStringLiteral("reason")).toString(),
            body.value(QStringLiteral("detail")).toString());
    });
}

} // namespace cutpilot::ipc
