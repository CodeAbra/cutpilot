#include <QtTest/QtTest>

#include <QApplication>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "ExportController.h"
#include "cutpilot/core/timeline/ProjectStore.h"
#include "cutpilot/ipc/ConvertClient.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/render/NodeLayerItem.h"

using namespace cutpilot;
using namespace cutpilot::core::timeline;

namespace {

QString writePng(const QString &path)
{
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(Qt::darkMagenta);
    image.save(path, "PNG");
    return path;
}

QString writeWorkflow(const QString &path)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(R"({
        "nodes": [
            {"id": 1, "type": "CLIPTextEncode", "pos": [0, 0],
             "widgets_values": ["dusk"]},
            {"id": 2, "type": "KSampler", "pos": [300, 0]}
        ],
        "links": [[1, 1, 0, 2, 1, "CONDITIONING"]]
    })");
    return path;
}

} // namespace

class ExportControllerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void exportsTheBoardHeadlessly();
    void resultsLandInTheProjectAsGeneratorSegments();
    void concurrentComfyImportIsRefusedAndTheFirstLandsOnce();

private:
    QTemporaryDir m_dir;
    ipc::SidecarHost m_host{ QStringLiteral("sidecars/convert"),
                             QStringLiteral("convert") };
    ipc::ConvertClient m_client;
    render::NodeLayerItem *m_layer = nullptr;
    ExportController *m_exporter = nullptr;
};

void ExportControllerTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(m_dir.isValid());

    QSignalSpy readySpy(&m_host, &ipc::SidecarHost::ready);
    QSignalSpy failedSpy(&m_host, &ipc::SidecarHost::failed);
    m_host.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1 || failedSpy.count() == 1,
                             15000);
    if (failedSpy.count() > 0)
        QFAIL(qPrintable(failedSpy.first().first().toString()));
    m_client.setEndpoint(m_host.port(), m_host.token());

    m_layer = new render::NodeLayerItem;

    core::Node generated;
    generated.kind = core::NodeKind::Generate;
    generated.title = QStringLiteral("Opening");
    generated.worldPos = QPointF(0, 0);
    generated.worldSize = QSizeF(280, 200);
    generated.resultPath =
        writePng(m_dir.filePath(QStringLiteral("opening.png")));
    m_layer->graph().addNode(generated);

    m_exporter = new ExportController(m_layer, &m_client, nullptr, m_layer);
}

void ExportControllerTest::cleanupTestCase()
{
    delete m_layer;
    m_host.stop();
}

void ExportControllerTest::exportsTheBoardHeadlessly()
{
    QSignalSpy finished(m_exporter, &ExportController::exportFinished);
    const QString directory = m_dir.filePath(QStringLiteral("bundle"));
    m_exporter->exportTimelineTo(directory, QStringLiteral("cut"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QVERIFY(finished.first().at(0).toBool());

    const ExportBundle &bundle = m_exporter->lastBundle();
    QVERIFY(QFileInfo::exists(bundle.edlPath));
    QVERIFY(QFileInfo::exists(bundle.fcpxmlPath));
    QVERIFY(QFileInfo::exists(bundle.otioPath));
    QVERIFY(QFileInfo::exists(bundle.directory + QStringLiteral("/media/opening.png")));
}

void ExportControllerTest::resultsLandInTheProjectAsGeneratorSegments()
{
    QFile::remove(m_exporter->projectPath());
    const QString path = m_exporter->addResultsToTimeline();
    QVERIFY(!path.isEmpty());

    TimelineProject project;
    QString error;
    QVERIFY2(loadProject(path, &project, &error), qPrintable(error));
    QCOMPARE(project.sequences.size(), 1);
    const Track &track = project.sequences[0].tracks[0];
    QCOMPARE(track.segments.size(), 1);
    QCOMPARE(track.segments[0].type, SegmentType::Generator);

    // A second landing appends after the first and survives a reload.
    QVERIFY(!m_exporter->addResultsToTimeline().isEmpty());
    QVERIFY2(loadProject(path, &project, &error), qPrintable(error));
    QCOMPARE(project.sequences[0].tracks[0].segments.size(), 2);
    QCOMPARE(project.sequences[0].tracks[0].segments[1].timelineIn,
             project.sequences[0].tracks[0].segments[0].timelineOut);
}

void ExportControllerTest::concurrentComfyImportIsRefusedAndTheFirstLandsOnce()
{
    const QString workflow =
        writeWorkflow(m_dir.filePath(QStringLiteral("workflow.json")));

    int importsLanded = 0;
    connect(m_exporter, &ExportController::comfyImportFinished, this,
            [&importsLanded](const core::ComfyImportOutcome &) {
                ++importsLanded;
            });
    QSignalSpy status(m_exporter, &ExportController::statusChanged);

    const int nodesBefore = m_layer->graph().nodes().size();
    m_exporter->importComfyFromFile(workflow, nullptr);
    m_exporter->importComfyFromFile(workflow, nullptr); // while in flight

    QTRY_COMPARE_WITH_TIMEOUT(importsLanded, 1, 10000);
    QCOMPARE(m_layer->graph().nodes().size(), nodesBefore + 2);

    bool refused = false;
    for (const QList<QVariant> &args : status) {
        if (args.first().toString().contains(QStringLiteral("already running")))
            refused = true;
    }
    QVERIFY(refused);

    // The guard resets once the first import settles: a follow-up import
    // runs and lands its own nodes.
    m_exporter->importComfyFromFile(workflow, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(importsLanded, 2, 10000);
    QCOMPARE(m_layer->graph().nodes().size(), nodesBefore + 4);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    ExportControllerTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_exportcontroller.moc"
