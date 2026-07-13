#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace cutpilot::ipc {

// The typed async client for the convert service: timeline interchange
// writers (FCPXML/OTIO), the ComfyUI workflow mapper, and the DaVinci
// Resolve import bridge. Every call returns immediately and reports through
// a signal on this object's thread, so a caller on the GUI thread never
// blocks on the service.
class ConvertClient : public QObject {
    Q_OBJECT

public:
    explicit ConvertClient(QObject *parent = nullptr);

    // Point the client at a running service. Until this is called the
    // client is not ready and every request fails with "unavailable".
    void setEndpoint(quint16 port, const QByteArray &token);
    bool ready() const { return m_port != 0; }

    // Write the timeline payload as the given interchange format —
    // "fcpxml" or "otio". The payload carries the title, rate, shots, and
    // the absolute output path (see the sidecar's payload contract).
    void exportTimeline(const QString &format, const QJsonObject &payload);

    // Map a ComfyUI workflow document into canvas node specs with the
    // per-node tier report.
    void importComfyWorkflow(const QJsonObject &workflow);

    // Map canvas node specs back into a ComfyUI workflow document.
    void exportComfyWorkflow(const QJsonArray &nodes,
                             const QJsonArray &connections);

    // Hand an interchange file to a running DaVinci Resolve.
    void importIntoResolve(const QString &path);

signals:
    void timelineExported(const QString &format, const QString &path);
    void timelineExportFailed(const QString &format, const QString &error);
    void comfyImported(const QJsonObject &result);
    void comfyImportFailed(const QString &error);
    void comfyExported(const QJsonObject &workflow);
    void comfyExportFailed(const QString &error);
    void resolveImportFinished(bool ok, const QString &reason,
                               const QString &detail);

private:
    QNetworkReply *post(const QString &path, const QJsonObject &body);
    static QString replyError(QNetworkReply *reply, const QJsonObject &body);

    QNetworkAccessManager *m_network = nullptr;
    quint16 m_port = 0;
    QByteArray m_token;
};

} // namespace cutpilot::ipc
