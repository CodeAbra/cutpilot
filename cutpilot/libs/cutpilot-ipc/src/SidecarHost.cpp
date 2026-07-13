#include "cutpilot/ipc/SidecarHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace cutpilot::ipc {

namespace {

constexpr int kHealthAttempts = 60;
constexpr int kHealthIntervalMs = 150;
constexpr int kMaxAnnounceBufferBytes = 64 * 1024;
const char kListeningPrefix[] = "CUTPILOT_LISTENING ";

QByteArray freshToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / int(sizeof(quint32)));
    return raw.toHex();
}

} // namespace

SidecarHost::SidecarHost(QObject *parent)
    : SidecarHost(QStringLiteral("sidecars/generation"),
                  QStringLiteral("generation"), parent)
{
}

SidecarHost::SidecarHost(const QString &relativeDir, const QString &label,
                         QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_network(new QNetworkAccessManager(this))
    , m_healthTimer(new QTimer(this))
    , m_relativeDir(relativeDir)
    , m_label(label)
{
    m_healthTimer->setInterval(kHealthIntervalMs);
    connect(m_healthTimer, &QTimer::timeout, this, &SidecarHost::probeHealth);
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            &SidecarHost::onStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this,
            &SidecarHost::onStandardError);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        fail(QStringLiteral("The %1 service failed to launch: %2")
                 .arg(m_label, m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!m_settled)
            fail(QStringLiteral("The %1 service exited during startup (code %2)")
                     .arg(m_label)
                     .arg(code));
    });
}

SidecarHost::~SidecarHost()
{
    stop();
}

QString SidecarHost::locatePython() const
{
    const QString override = qEnvironmentVariable("CUTPILOT_PYTHON");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::findExecutable(QStringLiteral("python3"));
}

QString SidecarHost::locateEntryScript() const
{
    // The override points at one service's directory; it only stands in for
    // the service it was aimed at, never for every host in the process.
    const QString override = qEnvironmentVariable("CUTPILOT_SIDECAR_DIR");
    if (!override.isEmpty()
        && QDir(override).dirName() == QDir(m_relativeDir).dirName()) {
        const QString candidate = QDir(override).filePath(QStringLiteral("main.py"));
        return QFile::exists(candidate) ? candidate : QString();
    }

    // In a development tree the service sources sit beside the app sources;
    // walk up from the executable until the layout is found.
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 10; ++depth) {
        const QString candidate =
            dir.filePath(m_relativeDir + QStringLiteral("/main.py"));
        if (QFile::exists(candidate))
            return candidate;
        if (!dir.cdUp())
            break;
    }
    return QString();
}

void SidecarHost::start()
{
    m_settled = false;
    m_port = 0;
    m_stdoutBuffer.clear();

    const QString python = locatePython();
    if (python.isEmpty()) {
        fail(QStringLiteral("No python3 interpreter found for the %1 service")
                 .arg(m_label));
        return;
    }
    const QString script = locateEntryScript();
    if (script.isEmpty()) {
        fail(QStringLiteral("The %1 service sources were not found").arg(m_label));
        return;
    }

    m_token = freshToken();

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("CUTPILOT_IPC_TOKEN"),
                       QString::fromLatin1(m_token));

    // Only the generation service writes media; it alone gets a media
    // directory prepared and announced.
    if (m_relativeDir == QStringLiteral("sidecars/generation")) {
        m_generationDir = qEnvironmentVariable("CUTPILOT_GEN_DIR");
        if (m_generationDir.isEmpty()) {
            m_generationDir = QStandardPaths::writableLocation(
                                  QStandardPaths::AppLocalDataLocation)
                + QStringLiteral("/generations");
        }
        QDir().mkpath(m_generationDir);
        environment.insert(QStringLiteral("CUTPILOT_GEN_DIR"), m_generationDir);
    }
    m_process->setProcessEnvironment(environment);

    m_process->start(python, { QStringLiteral("-u"), script });
}

void SidecarHost::stop()
{
    m_healthTimer->stop();
    if (m_process->state() == QProcess::NotRunning)
        return;
    m_settled = true;
    m_process->terminate();
    if (!m_process->waitForFinished(1500))
        m_process->kill();
}

void SidecarHost::onStandardOutput()
{
    m_stdoutBuffer += m_process->readAllStandardOutput();
    if (m_port == 0 && m_stdoutBuffer.size() > kMaxAnnounceBufferBytes) {
        m_stdoutBuffer.clear();
        fail(QStringLiteral("The %1 service produced no port announcement")
                 .arg(m_label));
        return;
    }
    int newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) != -1) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (m_port == 0 && line.startsWith(kListeningPrefix)) {
            bool ok = false;
            const int port = line.mid(int(sizeof(kListeningPrefix)) - 1).toInt(&ok);
            if (ok && port > 0 && port <= 65535) {
                m_port = quint16(port);
                beginHealthGate();
            }
        }
    }
}

void SidecarHost::onStandardError()
{
    const QList<QByteArray> lines = m_process->readAllStandardError().split('\n');
    for (const QByteArray &line : lines) {
        if (!line.trimmed().isEmpty())
            qWarning("%s sidecar: %s", qPrintable(m_label), line.constData());
    }
}

void SidecarHost::beginHealthGate()
{
    m_healthAttemptsLeft = kHealthAttempts;
    m_healthTimer->start();
    probeHealth();
}

void SidecarHost::probeHealth()
{
    if (m_settled) {
        m_healthTimer->stop();
        return;
    }
    if (--m_healthAttemptsLeft < 0) {
        m_healthTimer->stop();
        fail(QStringLiteral("The %1 service did not become healthy").arg(m_label));
        return;
    }

    QNetworkRequest request(
        QUrl(QStringLiteral("http://127.0.0.1:%1/health").arg(m_port)));
    request.setRawHeader("Authorization", "Bearer " + m_token);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_settled || reply->error() != QNetworkReply::NoError)
            return; // the timer retries until attempts run out
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (body.value(QStringLiteral("status")).toString()
            != QStringLiteral("serving"))
            return;
        m_settled = true;
        m_healthTimer->stop();
        emit ready(m_port, m_token);
    });
}

void SidecarHost::fail(const QString &reason)
{
    if (m_settled)
        return;
    m_settled = true;
    m_healthTimer->stop();
    emit failed(reason);
}

} // namespace cutpilot::ipc
