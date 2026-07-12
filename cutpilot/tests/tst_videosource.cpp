#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/CompositorService.h"
#include "cutpilot/render/NodeLayerItem.h"

using namespace cutpilot;
using core::NodeKind;
using render::CompositorService;

namespace {

// The mean color over the image center, tolerant of codec edges.
QColor centerColor(const QImage &image)
{
    const int x0 = image.width() / 4;
    const int y0 = image.height() / 4;
    qint64 r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < image.height() - y0; ++y) {
        for (int x = x0; x < image.width() - x0; ++x) {
            const QColor c = image.pixelColor(x, y);
            r += c.red();
            g += c.green();
            b += c.blue();
            ++n;
        }
    }
    return n > 0 ? QColor(int(r / n), int(g / n), int(b / n)) : QColor();
}

} // namespace

class VideoSourceTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void framesArriveAtProxyResolution();
    void scrubbingSeeksWithoutBlocking();
    void playbackAdvancesOnItsOwn();

private:
    QTemporaryDir m_dir;
    QString m_videoPath;

    struct Rig {
        render::CanvasController camera;
        render::NodeLayerItem layer;
        CompositorService media;
        int videoId = -1;

        explicit Rig(const QString &path)
        {
            layer.setSize(QSizeF(1600, 1000));
            layer.setController(&camera);
            core::Node video = core::compositeNodePrototype(NodeKind::Video);
            video.mediaPath = path;
            videoId = layer.graph().addNode(video);
            media.setLayer(&layer);
            media.scheduleRefresh();
        }
    };
};

void VideoSourceTest::initTestCase()
{
    // A deterministic asset: one second of red, then one second of blue, so
    // the first frame and a late seek have known colors.
    QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        ffmpeg = QStandardPaths::findExecutable(
            QStringLiteral("ffmpeg"),
            { QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin") });
    }
    if (ffmpeg.isEmpty())
        QSKIP("No ffmpeg on this machine to generate the test video");

    QVERIFY(m_dir.isValid());
    m_videoPath = m_dir.filePath(QStringLiteral("two-colors.mp4"));

    QProcess encoder;
    encoder.start(ffmpeg,
                  { QStringLiteral("-y"), QStringLiteral("-f"),
                    QStringLiteral("lavfi"), QStringLiteral("-i"),
                    QStringLiteral("color=red:s=128x96:d=1:r=25"),
                    QStringLiteral("-f"), QStringLiteral("lavfi"),
                    QStringLiteral("-i"),
                    QStringLiteral("color=blue:s=128x96:d=1:r=25"),
                    QStringLiteral("-filter_complex"),
                    QStringLiteral("[0][1]concat=n=2:v=1:a=0"),
                    QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                    m_videoPath });
    QVERIFY(encoder.waitForFinished(30000));
    QCOMPARE(encoder.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_videoPath));
}

void VideoSourceTest::framesArriveAtProxyResolution()
{
    Rig rig(m_videoPath);

    // The pre-roll frame lands without any transport interaction, decoded by
    // the media stack's own threads.
    QTRY_VERIFY_WITH_TIMEOUT(!rig.layer.nodeMediaImage(rig.videoId).isNull(),
                             15000);
    const QImage frame = rig.layer.nodeMediaImage(rig.videoId);
    QVERIFY(frame.width() <= 640);
    QVERIFY(frame.height() <= 640);

    const QColor color = centerColor(frame);
    QVERIFY2(color.red() > 180 && color.blue() < 90,
             qPrintable(QStringLiteral("first frame should be red, got %1")
                            .arg(color.name())));
}

void VideoSourceTest::scrubbingSeeksWithoutBlocking()
{
    Rig rig(m_videoPath);
    QTRY_VERIFY_WITH_TIMEOUT(!rig.layer.nodeMediaImage(rig.videoId).isNull(),
                             15000);
    QTRY_VERIFY_WITH_TIMEOUT(rig.media.videoDurationMs(rig.videoId) > 0, 15000);

    // The scrub call itself returns immediately — the seek happens on the
    // media stack's threads and the frame arrives through the event loop.
    QElapsedTimer clock;
    clock.start();
    rig.media.scrubVideo(rig.videoId, 0.75);
    QVERIFY2(clock.elapsed() < 50,
             qPrintable(QStringLiteral("scrub blocked for %1 ms")
                            .arg(clock.elapsed())));

    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const QColor c = centerColor(rig.layer.nodeMediaImage(rig.videoId));
            return c.blue() > 180 && c.red() < 90;
        }(),
        15000);
}

void VideoSourceTest::playbackAdvancesOnItsOwn()
{
    Rig rig(m_videoPath);
    QTRY_VERIFY_WITH_TIMEOUT(rig.media.videoDurationMs(rig.videoId) > 0, 15000);

    rig.media.setVideoPlaying(rig.videoId, true);
    QVERIFY(rig.media.videoPlaying(rig.videoId));
    QTRY_VERIFY_WITH_TIMEOUT(rig.media.videoPositionMs(rig.videoId) > 150, 15000);

    rig.media.setVideoPlaying(rig.videoId, false);
    QVERIFY(!rig.media.videoPlaying(rig.videoId));
}

int main(int argc, char *argv[])
{
    // The platform's media backend stalls at loading under the offscreen
    // platform, so this suite runs on the default platform instead — it
    // opens no windows, only a player and a sink.
    QGuiApplication app(argc, argv);
    VideoSourceTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_videosource.moc"
