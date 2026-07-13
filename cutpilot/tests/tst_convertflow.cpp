#include <QtTest/QtTest>

#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/timeline/ExportBundle.h"
#include "cutpilot/ipc/ConvertClient.h"
#include "cutpilot/ipc/SidecarHost.h"

using namespace cutpilot;
using namespace cutpilot::core::timeline;

namespace {

QString writePng(const QString &path, const QColor &color)
{
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(color);
    image.save(path, "PNG");
    return path;
}

// A two-shot board: a generation result on the left, a still on the right.
core::NodeGraph exportableGraph(const QString &mediaDir)
{
    core::NodeGraph graph;

    core::Node generated;
    generated.kind = core::NodeKind::Generate;
    generated.title = QStringLiteral("Opening");
    generated.worldPos = QPointF(0, 0);
    generated.resultPath = writePng(mediaDir + QStringLiteral("/opening.png"),
                                    Qt::darkBlue);
    generated.resultWidth = 64;
    generated.resultHeight = 48;
    graph.addNode(generated);

    core::Node still;
    still.kind = core::NodeKind::Still;
    still.title = QStringLiteral("Backdrop");
    still.worldPos = QPointF(400, 0);
    still.mediaPath =
        writePng(mediaDir + QStringLiteral("/backdrop.png"), Qt::darkGreen);
    graph.addNode(still);

    return graph;
}

QJsonObject comfyWorkflow()
{
    return QJsonDocument::fromJson(R"({
        "nodes": [
            {"id": 1, "type": "CLIPTextEncode", "pos": [80, 40],
             "widgets_values": ["a lighthouse at dusk"]},
            {"id": 2, "type": "KSampler", "pos": [400, 40]},
            {"id": 3, "type": "GlitterStorm", "pos": [700, 40],
             "properties": {"sparkle": true}}
        ],
        "links": [[1, 1, 0, 2, 1, "CONDITIONING"]]
    })")
        .object();
}

} // namespace

class ConvertFlowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void bundleLaysMediaAndEdlOnDisk();
    void sidecarWritesParseableFcpxml();
    void sidecarWritesParseableOtio();
    void comfyImportReportsTiersOverTheWire();
    void resolveImportRefusesWithoutResolve();

private:
    QTemporaryDir m_dir;
    ipc::SidecarHost m_host{ QStringLiteral("sidecars/convert"),
                             QStringLiteral("convert") };
    ipc::ConvertClient m_client;
    ExportBundle m_bundle;
};

void ConvertFlowTest::initTestCase()
{
    QVERIFY(m_dir.isValid());

    // The sidecar inherits this environment: aim the Resolve bridge at a
    // directory holding no scripting modules, so its refusal is
    // deterministic on machines with Resolve installed too.
    qputenv("CUTPILOT_RESOLVE_MODULES", "/nonexistent/resolve-modules");

    QSignalSpy readySpy(&m_host, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&m_host, &ipc::SidecarHost::failed);
    m_host.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1,
                             15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    m_client.setEndpoint(m_host.port(), m_host.token());
}

void ConvertFlowTest::cleanupTestCase()
{
    m_host.stop();
}

void ConvertFlowTest::bundleLaysMediaAndEdlOnDisk()
{
    QVERIFY(QDir(m_dir.path()).mkdir(QStringLiteral("sources")));
    QVERIFY(QDir(m_dir.path()).mkdir(QStringLiteral("bundle")));
    const core::NodeGraph graph =
        exportableGraph(m_dir.filePath(QStringLiteral("sources")));

    GraphTimelineOptions options;
    options.projectName = QStringLiteral("Canvas Cut");
    m_bundle = writeExportBundle(graph, options,
                                 m_dir.filePath(QStringLiteral("bundle")),
                                 QStringLiteral("cut"));
    QVERIFY2(m_bundle.ok, qPrintable(m_bundle.error));

    // The bundle is self-contained: media copied in, asset paths rewritten.
    QCOMPARE(m_bundle.project.assets.size(), 2);
    for (const MediaAsset &asset : m_bundle.project.assets) {
        QVERIFY(asset.filePath.startsWith(m_bundle.directory));
        QVERIFY(QFileInfo::exists(asset.filePath));
    }

    QVERIFY(QFileInfo::exists(m_bundle.edlPath));
    QFile edl(m_bundle.edlPath);
    QVERIFY(edl.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(edl.readAll());
    QVERIFY(text.startsWith(QStringLiteral("TITLE: Canvas Cut\n")));
    QVERIFY(text.contains(QStringLiteral("FCM: NON-DROP FRAME")));
    QVERIFY(text.contains(QStringLiteral("* FROM CLIP NAME: opening.png")));
}

void ConvertFlowTest::sidecarWritesParseableFcpxml()
{
    QVERIFY(m_bundle.ok);
    QSignalSpy exported(&m_client, &ipc::ConvertClient::timelineExported);
    QSignalSpy failed(&m_client, &ipc::ConvertClient::timelineExportFailed);
    m_client.exportTimeline(QStringLiteral("fcpxml"),
                            exportPayload(m_bundle, QStringLiteral("fcpxml")));
    QTRY_VERIFY_WITH_TIMEOUT(exported.count() == 1 || failed.count() == 1,
                             10000);
    if (failed.count() > 0)
        QFAIL(qPrintable(failed.first().at(1).toString()));
    QCOMPARE(exported.first().at(1).toString(), m_bundle.fcpxmlPath);

    QFile file(m_bundle.fcpxmlPath);
    QVERIFY(file.open(QIODevice::ReadOnly));

    // Walk the whole document with a strict parser; collect the structure.
    QXmlStreamReader xml(&file);
    QStringList assetIds;
    QStringList clipRefs;
    int formats = 0;
    int mediaReps = 0;
    QString version;
    const QRegularExpression rationalSeconds(
        QStringLiteral("^(0|\\d+/\\d+)s$"));
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        const auto name = xml.name();
        const QXmlStreamAttributes attributes = xml.attributes();
        if (name == QStringLiteral("fcpxml")) {
            version = attributes.value(QStringLiteral("version")).toString();
        } else if (name == QStringLiteral("format")) {
            ++formats;
            QVERIFY(rationalSeconds
                        .match(attributes.value(QStringLiteral("frameDuration"))
                                   .toString())
                        .hasMatch());
        } else if (name == QStringLiteral("asset")) {
            assetIds << attributes.value(QStringLiteral("id")).toString();
        } else if (name == QStringLiteral("media-rep")) {
            ++mediaReps;
            QVERIFY(attributes.value(QStringLiteral("src"))
                        .toString()
                        .startsWith(QStringLiteral("file://")));
        } else if (name == QStringLiteral("asset-clip")) {
            clipRefs << attributes.value(QStringLiteral("ref")).toString();
            for (const auto &key :
                 { QStringLiteral("offset"), QStringLiteral("duration"),
                   QStringLiteral("start") }) {
                QVERIFY2(rationalSeconds
                             .match(attributes.value(key).toString())
                             .hasMatch(),
                         qPrintable(key + QStringLiteral(" not rational")));
            }
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QCOMPARE(version, QStringLiteral("1.11"));
    QCOMPARE(formats, 1);
    QCOMPARE(assetIds.size(), 2);
    QCOMPARE(mediaReps, 2);
    QCOMPARE(clipRefs.size(), 2);
    for (const QString &ref : clipRefs)
        QVERIFY2(assetIds.contains(ref), qPrintable(ref));
}

void ConvertFlowTest::sidecarWritesParseableOtio()
{
    QVERIFY(m_bundle.ok);
    QSignalSpy exported(&m_client, &ipc::ConvertClient::timelineExported);
    QSignalSpy failed(&m_client, &ipc::ConvertClient::timelineExportFailed);
    m_client.exportTimeline(QStringLiteral("otio"),
                            exportPayload(m_bundle, QStringLiteral("otio")));
    QTRY_VERIFY_WITH_TIMEOUT(exported.count() == 1 || failed.count() == 1,
                             10000);
    if (failed.count() > 0)
        QFAIL(qPrintable(failed.first().at(1).toString()));

    QFile file(m_bundle.otioPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);

    const QJsonObject timeline = document.object();
    QCOMPARE(timeline.value(QStringLiteral("OTIO_SCHEMA")).toString(),
             QStringLiteral("Timeline.1"));
    const QJsonObject stack =
        timeline.value(QStringLiteral("tracks")).toObject();
    QCOMPARE(stack.value(QStringLiteral("OTIO_SCHEMA")).toString(),
             QStringLiteral("Stack.1"));
    const QJsonArray tracks =
        stack.value(QStringLiteral("children")).toArray();
    QCOMPARE(tracks.size(), 1);
    const QJsonObject track = tracks.first().toObject();
    QCOMPARE(track.value(QStringLiteral("OTIO_SCHEMA")).toString(),
             QStringLiteral("Track.1"));
    QCOMPARE(track.value(QStringLiteral("kind")).toString(),
             QStringLiteral("Video"));

    const QJsonArray children =
        track.value(QStringLiteral("children")).toArray();
    QCOMPARE(children.size(), 2);
    for (const QJsonValue &value : children) {
        const QJsonObject clip = value.toObject();
        QCOMPARE(clip.value(QStringLiteral("OTIO_SCHEMA")).toString(),
                 QStringLiteral("Clip.2"));
        const QString key =
            clip.value(QStringLiteral("active_media_reference_key")).toString();
        const QJsonObject reference =
            clip.value(QStringLiteral("media_references"))
                .toObject()
                .value(key)
                .toObject();
        QCOMPARE(reference.value(QStringLiteral("OTIO_SCHEMA")).toString(),
                 QStringLiteral("ExternalReference.1"));
        QVERIFY(reference.value(QStringLiteral("target_url"))
                    .toString()
                    .startsWith(QStringLiteral("file://")));
        const QJsonObject range =
            clip.value(QStringLiteral("source_range")).toObject();
        QCOMPARE(range.value(QStringLiteral("OTIO_SCHEMA")).toString(),
                 QStringLiteral("TimeRange.1"));
    }

    // The generation result is marked as produced media; the still is not.
    QCOMPARE(children.at(0)
                 .toObject()
                 .value(QStringLiteral("metadata"))
                 .toObject()
                 .value(QStringLiteral("cutpilot"))
                 .toObject()
                 .value(QStringLiteral("generator"))
                 .toBool(),
             true);
    QCOMPARE(children.at(1)
                 .toObject()
                 .value(QStringLiteral("metadata"))
                 .toObject()
                 .value(QStringLiteral("cutpilot"))
                 .toObject()
                 .value(QStringLiteral("generator"))
                 .toBool(),
             false);
}

void ConvertFlowTest::comfyImportReportsTiersOverTheWire()
{
    QSignalSpy imported(&m_client, &ipc::ConvertClient::comfyImported);
    QSignalSpy failed(&m_client, &ipc::ConvertClient::comfyImportFailed);
    m_client.importComfyWorkflow(comfyWorkflow());
    QTRY_VERIFY_WITH_TIMEOUT(imported.count() == 1 || failed.count() == 1,
                             10000);
    if (failed.count() > 0)
        QFAIL(qPrintable(failed.first().first().toString()));

    const QJsonObject result = imported.first().first().toJsonObject();
    const QJsonArray report = result.value(QStringLiteral("report")).toArray();
    QCOMPARE(report.size(), 3);
    QHash<int, QString> tiers;
    for (const QJsonValue &value : report) {
        const QJsonObject row = value.toObject();
        tiers.insert(row.value(QStringLiteral("id")).toInt(),
                     row.value(QStringLiteral("tier")).toString());
    }
    QCOMPARE(tiers.value(1), QStringLiteral("exact"));
    QCOMPARE(tiers.value(2), QStringLiteral("substituted"));
    QCOMPARE(tiers.value(3), QStringLiteral("passthrough"));

    // The unknown node arrives preserved, original payload intact.
    const QJsonArray nodes = result.value(QStringLiteral("nodes")).toArray();
    QCOMPARE(nodes.size(), 3);
    for (const QJsonValue &value : nodes) {
        const QJsonObject node = value.toObject();
        if (node.value(QStringLiteral("comfy_id")).toInt() != 3)
            continue;
        const QJsonObject opaque =
            node.value(QStringLiteral("opaque")).toObject();
        QCOMPARE(opaque.value(QStringLiteral("type")).toString(),
                 QStringLiteral("GlitterStorm"));
    }
}

void ConvertFlowTest::resolveImportRefusesWithoutResolve()
{
    QSignalSpy finished(&m_client, &ipc::ConvertClient::resolveImportFinished);
    m_client.importIntoResolve(m_dir.filePath(QStringLiteral("missing.fcpxml")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QCOMPARE(finished.first().at(1).toString(),
             QStringLiteral("file_not_found"));

    // An existing file still refuses cleanly when Resolve is absent.
    QVERIFY(m_bundle.ok);
    finished.clear();
    m_client.importIntoResolve(m_bundle.fcpxmlPath);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QCOMPARE(finished.first().at(1).toString(),
             QStringLiteral("resolve_unavailable"));
    QVERIFY(!finished.first().at(2).toString().isEmpty());
}

QTEST_GUILESS_MAIN(ConvertFlowTest)
#include "tst_convertflow.moc"
