#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QTimer;
QT_END_NAMESPACE

namespace cutpilot::ipc {

// Owns the generation service process: spawn, announce, health-gate, reap.
// The host generates a fresh channel token per launch and hands it to the
// child through its environment; ready() fires only after the service both
// announced its port and answered a health check, so the first job is never
// raced against startup. The service is terminated with the host.
class SidecarHost : public QObject {
    Q_OBJECT

public:
    explicit SidecarHost(QObject *parent = nullptr);
    ~SidecarHost() override;

    // Spawn and begin the announce/health-gate sequence. Emits ready or
    // failed exactly once per start.
    void start();
    void stop();

    quint16 port() const { return m_port; }
    QByteArray token() const { return m_token; }

    // Where the service writes generated media.
    QString generationDir() const { return m_generationDir; }

signals:
    void ready(quint16 port, const QByteArray &token);
    void failed(const QString &reason);

private:
    QString locatePython() const;
    QString locateEntryScript() const;
    void onStandardOutput();
    void onStandardError();
    void beginHealthGate();
    void probeHealth();
    void fail(const QString &reason);

    QProcess *m_process = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_healthTimer = nullptr;
    QByteArray m_stdoutBuffer;
    QByteArray m_token;
    QString m_generationDir;
    quint16 m_port = 0;
    int m_healthAttemptsLeft = 0;
    bool m_settled = false; // ready or failed already emitted for this start
};

} // namespace cutpilot::ipc
