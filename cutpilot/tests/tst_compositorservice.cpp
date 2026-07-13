#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/CompositorEngine.h"
#include "cutpilot/render/CompositorService.h"
#include "cutpilot/render/NodeLayerItem.h"

using namespace cutpilot;
using core::NodeKind;

namespace {

void wire(core::NodeGraph &graph, int from, int fromPort, int to, int toPort)
{
    core::Connection edge;
    edge.fromNodeId = from;
    edge.fromPortIndex = fromPort;
    edge.toNodeId = to;
    edge.toPortIndex = toPort;
    QVERIFY(graph.addConnection(edge) != -1);
}

QImage uniformImage(int w, int h, const QColor &color)
{
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

// The mean color over the image center, tolerant of pass edges.
QColor centerColor(const QImage &image)
{
    if (image.isNull())
        return QColor();
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

// Spin the event loop in short turns until the condition holds, recording the
// longest single turn — the largest stretch the GUI thread was unavailable.
template <typename Condition>
qint64 longestTurnUntil(Condition condition, int timeoutMs)
{
    QElapsedTimer deadline;
    deadline.start();
    qint64 longest = 0;
    while (!condition() && deadline.elapsed() < timeoutMs) {
        QElapsedTimer turn;
        turn.start();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        longest = qMax(longest, qint64(turn.elapsed()));
    }
    return longest;
}

} // namespace

class CompositorServiceTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void thumbnailsLandWithoutStallingTheGuiThread();

private:
    // A GUI-thread turn longer than this while thumbnails render counts as a
    // stall; a populated board rendering inline blocks for hundreds of ms.
    static constexpr qint64 kStallBudgetMs = 40;
};

void CompositorServiceTest::initTestCase()
{
    render::CompositorEngine probe;
    if (!probe.adoptHeadlessDevice())
        QSKIP("No windowless GPU device on this platform");
}

void CompositorServiceTest::thumbnailsLandWithoutStallingTheGuiThread()
{
    render::CanvasController camera;
    render::NodeLayerItem layer;
    layer.setSize(QSizeF(1600, 1000));
    layer.setController(&camera);

    // A long compositing chain over a large source: enough per-node scale,
    // render, and readback work that doing it inline would freeze the GUI
    // thread for far longer than one frame.
    core::NodeGraph &graph = layer.graph();
    const int stillId =
        graph.addNode(core::compositeNodePrototype(NodeKind::Still));
    layer.setNodeMedia(stillId, uniformImage(2048, 2048, Qt::red));

    constexpr int kChainLength = 60;
    QVector<int> blends;
    int upstream = stillId;
    int upstreamPort = 0; // the still's image output
    for (int i = 0; i < kChainLength; ++i) {
        const int blend =
            graph.addNode(core::compositeNodePrototype(NodeKind::Blend));
        wire(graph, upstream, upstreamPort, blend, 0);
        blends.push_back(blend);
        upstream = blend;
        upstreamPort = 2; // a blend's result output
    }
    const int terminal = blends.last();

    render::CompositorService media;
    media.setLayer(&layer);
    media.scheduleRefresh();

    // Every thumbnail arrives, and no single event-loop turn on this thread
    // was blocked past the stall budget while they rendered.
    const qint64 longest = longestTurnUntil(
        [&] {
            for (int blend : blends) {
                if (layer.nodeMediaImage(blend).isNull())
                    return false;
            }
            return true;
        },
        30000);
    QVERIFY(!layer.nodeMediaImage(terminal).isNull());
    QVERIFY2(longest < kStallBudgetMs,
             qPrintable(
                 QStringLiteral("GUI thread blocked for %1 ms").arg(longest)));

    // The pixels are the real composite: an unwired over input passes the
    // base through, so the chain ends red.
    const QColor first = centerColor(layer.nodeMediaImage(terminal));
    QVERIFY2(first.red() > 180 && first.blue() < 60,
             qPrintable(QStringLiteral("terminal thumbnail should be red, "
                                       "got %1")
                            .arg(first.name())));

    // New source pixels re-render the chain through the same path: swap the
    // still to blue and the terminal thumbnail follows.
    layer.setNodeMedia(stillId, uniformImage(512, 512, Qt::blue));
    media.scheduleRefresh();
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const QColor c = centerColor(layer.nodeMediaImage(terminal));
            return c.blue() > 180 && c.red() < 60;
        }(),
        30000);
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication app(argc, argv);
    CompositorServiceTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_compositorservice.moc"
