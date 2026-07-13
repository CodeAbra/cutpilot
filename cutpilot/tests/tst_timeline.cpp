#include <QtTest/QtTest>

#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/timeline/GraphTimeline.h"
#include "cutpilot/core/timeline/ProjectStore.h"
#include "cutpilot/core/timeline/Timeline.h"

using namespace cutpilot::core;
using namespace cutpilot::core::timeline;

namespace {

MediaAsset sampleAsset(const QString &id, const QString &path)
{
    MediaAsset asset;
    asset.id = id;
    asset.filePath = path;
    asset.name = QFileInfo(path).fileName();
    asset.fps = Fps{ 24, 1 };
    asset.durationFrames = 96;
    asset.width = 1920;
    asset.height = 1080;
    return asset;
}

Segment sampleSegment(const QString &id, const QString &assetId, qint64 in,
                      qint64 out, SegmentType type = SegmentType::Clip)
{
    Segment segment;
    segment.id = id;
    segment.type = type;
    segment.assetId = assetId;
    segment.timelineIn = in;
    segment.timelineOut = out;
    segment.sourceIn = 0;
    segment.sourceOut = out - in;
    return segment;
}

TimelineProject sampleProject()
{
    TimelineProject project;
    project.name = QStringLiteral("Sample");
    project.assets = { sampleAsset(QStringLiteral("a1"),
                                   QStringLiteral("/media/shot-one.png")),
                       sampleAsset(QStringLiteral("a2"),
                                   QStringLiteral("/media/shot-two.png")) };

    Track track;
    track.name = QStringLiteral("V1");
    track.segments = { sampleSegment(QStringLiteral("s1"),
                                     QStringLiteral("a1"), 0, 96,
                                     SegmentType::Generator),
                       sampleSegment(QStringLiteral("s2"),
                                     QStringLiteral("a2"), 96, 192) };

    Sequence sequence;
    sequence.name = QStringLiteral("Main");
    sequence.fps = Fps{ 24, 1 };
    sequence.tracks = { track };
    project.sequences = { sequence };
    return project;
}

QString writePng(const QString &path, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    image.save(path, "PNG");
    return path;
}

} // namespace

class TimelineTest : public QObject {
    Q_OBJECT

private slots:
    void timecodeCountsNonDropAtTheNominalRate();
    void validProjectValidates();
    void validationRejectsBrokenStructures();
    void projectJsonRoundTripsExactly();
    void storeRoundTripsAProjectThroughDisk();
    void generatorSegmentSurvivesSaveAndLoad();
    void appendUniquifiesCollidingAssetIds();
    void graphExportsTerminalMediaLeftToRight();
    void graphSkipsIngredientsAndUnrenderedComposites();
};

void TimelineTest::timecodeCountsNonDropAtTheNominalRate()
{
    const Fps film{ 24, 1 };
    QCOMPARE(smpteTimecode(0, film), QStringLiteral("00:00:00:00"));
    QCOMPARE(smpteTimecode(23, film), QStringLiteral("00:00:00:23"));
    QCOMPARE(smpteTimecode(24, film), QStringLiteral("00:00:01:00"));
    QCOMPARE(smpteTimecode(24 * 60, film), QStringLiteral("00:01:00:00"));
    QCOMPARE(smpteTimecode(24 * 3600 + 24 * 61 + 5, film),
             QStringLiteral("01:01:01:05"));
    QCOMPARE(smpteTimecode(-5, film), QStringLiteral("00:00:00:00"));

    // 23.976 counts at 24; 29.97 counts at 30 (non-drop).
    const Fps ntscFilm{ 24000, 1001 };
    const Fps ntsc{ 30000, 1001 };
    QCOMPARE(ntscFilm.nominalRate(), 24);
    QCOMPARE(smpteTimecode(24, ntscFilm), QStringLiteral("00:00:01:00"));
    QCOMPARE(smpteTimecode(30, ntsc), QStringLiteral("00:00:01:00"));
}

void TimelineTest::validProjectValidates()
{
    QVERIFY2(sampleProject().validate().isEmpty(),
             qPrintable(sampleProject().validate()));
}

void TimelineTest::validationRejectsBrokenStructures()
{
    TimelineProject project = sampleProject();
    project.sequences[0].tracks[0].segments[1].assetId =
        QStringLiteral("missing");
    QVERIFY(!project.validate().isEmpty());

    project = sampleProject();
    project.sequences[0].tracks[0].segments[1].timelineIn = 50;
    QVERIFY(!project.validate().isEmpty());

    project = sampleProject();
    project.sequences[0].tracks[0].segments[0].timelineOut = 0;
    QVERIFY(!project.validate().isEmpty());

    project = sampleProject();
    project.assets[1].id = project.assets[0].id;
    QVERIFY(!project.validate().isEmpty());

    project = sampleProject();
    project.sequences[0].fps = Fps{ 0, 1 };
    QVERIFY(!project.validate().isEmpty());

    Segment gap;
    gap.id = QStringLiteral("g");
    gap.type = SegmentType::Gap;
    gap.timelineIn = 0;
    gap.timelineOut = 10;
    QVERIFY(gap.validate().isEmpty());
    gap.assetId = QStringLiteral("a1");
    QVERIFY(!gap.validate().isEmpty());
}

void TimelineTest::projectJsonRoundTripsExactly()
{
    const TimelineProject project = sampleProject();
    const TimelineProject back = TimelineProject::fromJson(project.toJson());
    QCOMPARE(back, project);
}

void TimelineTest::storeRoundTripsAProjectThroughDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("project.json"));

    const TimelineProject project = sampleProject();
    QString error;
    QVERIFY2(saveProject(project, path, &error), qPrintable(error));

    TimelineProject loaded;
    QVERIFY2(loadProject(path, &loaded, &error), qPrintable(error));
    QCOMPARE(loaded, project);

    // A load refuses a file whose contents fail validation.
    TimelineProject broken = project;
    broken.sequences[0].tracks[0].segments[0].assetId =
        QStringLiteral("missing");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(broken.toJson()).toJson());
    file.close();
    QVERIFY(!loadProject(path, &loaded, &error));
    QVERIFY(!error.isEmpty());
}

void TimelineTest::generatorSegmentSurvivesSaveAndLoad()
{
    TimelineProject project;
    MediaAsset produced =
        sampleAsset(QStringLiteral("gen-7"),
                    QStringLiteral("/media/generated-lighthouse.png"));
    const QString segmentId = appendGeneratorSegment(project, produced);
    QVERIFY(!segmentId.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("project.json"));
    QString error;
    QVERIFY2(saveProject(project, path, &error), qPrintable(error));

    TimelineProject loaded;
    QVERIFY2(loadProject(path, &loaded, &error), qPrintable(error));
    QCOMPARE(loaded.sequences.size(), 1);
    QCOMPARE(loaded.sequences[0].tracks.size(), 1);
    const Track &track = loaded.sequences[0].tracks[0];
    QCOMPARE(track.type, TrackType::Video);
    QCOMPARE(track.segments.size(), 1);
    const Segment &segment = track.segments[0];
    QCOMPARE(segment.id, segmentId);
    QCOMPARE(segment.type, SegmentType::Generator);
    const MediaAsset *asset = loaded.assetById(segment.assetId);
    QVERIFY(asset);
    QCOMPARE(asset->filePath,
             QStringLiteral("/media/generated-lighthouse.png"));

    // A second result appends after the first, not over it.
    MediaAsset second = sampleAsset(QStringLiteral("gen-8"),
                                    QStringLiteral("/media/generated-dawn.png"));
    appendGeneratorSegment(loaded, second);
    const Track &grown = loaded.sequences[0].tracks[0];
    QCOMPARE(grown.segments.size(), 2);
    QCOMPARE(grown.segments[1].timelineIn, grown.segments[0].timelineOut);
    QVERIFY(loaded.validate().isEmpty());
}

void TimelineTest::appendUniquifiesCollidingAssetIds()
{
    TimelineProject project;
    const MediaAsset asset =
        sampleAsset(QStringLiteral("gen"), QStringLiteral("/media/a.png"));
    appendGeneratorSegment(project, asset);
    appendGeneratorSegment(project, asset);
    QCOMPARE(project.assets.size(), 2);
    QVERIFY(project.assets[0].id != project.assets[1].id);
    QVERIFY(project.validate().isEmpty());
}

void TimelineTest::graphExportsTerminalMediaLeftToRight()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString stillPath =
        writePng(dir.filePath(QStringLiteral("backdrop.png")), 320, 200);

    NodeGraph graph;

    Node right;
    right.kind = NodeKind::Generate;
    right.title = QStringLiteral("Shot two");
    right.worldPos = QPointF(900, 0);
    right.resultPath = QStringLiteral("/results/second.png");
    right.resultWidth = 768;
    right.resultHeight = 512;
    const int rightId = graph.addNode(right);

    Node left;
    left.kind = NodeKind::Generate;
    left.title = QStringLiteral("Shot one");
    left.worldPos = QPointF(100, 50);
    left.resultPath = QStringLiteral("/results/first.png");
    const int leftId = graph.addNode(left);

    Node still;
    still.kind = NodeKind::Still;
    still.title = QStringLiteral("Backdrop");
    still.worldPos = QPointF(500, 0);
    still.mediaPath = stillPath;
    const int stillId = graph.addNode(still);

    GraphTimelineOptions options;
    options.fps = Fps{ 24, 1 };
    options.durationOverrides.insert(rightId, 48);
    const GraphTimelineResult result = timelineFromGraph(graph, options);

    QVERIFY2(result.project.validate().isEmpty(),
             qPrintable(result.project.validate()));
    QCOMPARE(result.shotNodeIds, (QVector<int>{ leftId, stillId, rightId }));

    const Track &track = result.project.sequences[0].tracks[0];
    QCOMPARE(track.segments.size(), 3);

    // Generation results land as Generator segments, file media as Clips.
    QCOMPARE(track.segments[0].type, SegmentType::Generator);
    QCOMPARE(track.segments[1].type, SegmentType::Clip);
    QCOMPARE(track.segments[2].type, SegmentType::Generator);

    // Stills default to four seconds; the override wins where given.
    QCOMPARE(track.segments[0].durationFrames(), qint64(96));
    QCOMPARE(track.segments[1].durationFrames(), qint64(96));
    QCOMPARE(track.segments[2].durationFrames(), qint64(48));

    // Shots are contiguous from zero.
    QCOMPARE(track.segments[0].timelineIn, qint64(0));
    QCOMPARE(track.segments[1].timelineIn, track.segments[0].timelineOut);
    QCOMPARE(track.segments[2].timelineIn, track.segments[1].timelineOut);

    // The still's dimensions come from the image header; the generation
    // result's from the node.
    const MediaAsset *stillAsset =
        result.project.assetById(track.segments[1].assetId);
    QVERIFY(stillAsset);
    QCOMPARE(stillAsset->width, 320);
    QCOMPARE(stillAsset->height, 200);
    const MediaAsset *genAsset =
        result.project.assetById(track.segments[2].assetId);
    QVERIFY(genAsset);
    QCOMPARE(genAsset->width, 768);
    QCOMPARE(genAsset->height, 512);
}

void TimelineTest::graphSkipsIngredientsAndUnrenderedComposites()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    NodeGraph graph;

    // A still feeding a blend is an ingredient, not a shot.
    Node still;
    still.kind = NodeKind::Still;
    still.mediaPath = writePng(dir.filePath(QStringLiteral("base.png")), 64, 64);
    still.ports = { { QStringLiteral("image"), PortType::Image, false, 0.5 } };
    const int stillId = graph.addNode(still);

    Node blend;
    blend.kind = NodeKind::Blend;
    blend.title = QStringLiteral("Blend");
    blend.ports = { { QStringLiteral("base"), PortType::Image, true, 0.3 },
                    { QStringLiteral("over"), PortType::Image, true, 0.7 },
                    { QStringLiteral("out"), PortType::Image, false, 0.5 } };
    const int blendId = graph.addNode(blend);

    Connection wire;
    wire.fromNodeId = stillId;
    wire.fromPortIndex = 0;
    wire.toNodeId = blendId;
    wire.toPortIndex = 0;
    QVERIFY(graph.addConnection(wire) != -1);

    // Without a rendered file the composite terminal is skipped, with a
    // reason naming it.
    GraphTimelineOptions options;
    GraphTimelineResult result = timelineFromGraph(graph, options);
    QVERIFY(result.shotNodeIds.isEmpty());
    QCOMPARE(result.skipped.size(), 1);
    QVERIFY(result.skipped[0].contains(QStringLiteral("Blend")));

    // With one, it exports as produced media.
    options.renderedMedia.insert(
        blendId, writePng(dir.filePath(QStringLiteral("out.png")), 64, 64));
    result = timelineFromGraph(graph, options);
    QCOMPARE(result.shotNodeIds, (QVector<int>{ blendId }));
    QCOMPARE(result.project.sequences[0].tracks[0].segments[0].type,
             SegmentType::Generator);
    QVERIFY(result.skipped.isEmpty());
}

QTEST_GUILESS_MAIN(TimelineTest)
#include "tst_timeline.moc"
