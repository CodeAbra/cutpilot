#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/CanvasItem.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

using cutpilot::render::CanvasController;
using cutpilot::render::CanvasItem;
using cutpilot::render::NodeLayerItem;
using cutpilot::theme::ThemeTable;

namespace {

// A frameless live readout pinned to the bottom-left of the canvas, mirroring the
// design's bottom-left cluster. It shows the current zoom as a percentage in the
// monospace numeric style.
class ZoomReadout : public QLabel {
public:
    explicit ZoomReadout(const ThemeTable &theme, QWidget *parent)
        : QLabel(parent)
    {
        QFont mono;
        mono.setFamilies({ QStringLiteral("SF Mono"),
                           QStringLiteral("Menlo"),
                           QStringLiteral("Cascadia Code") });
        mono.setStyleHint(QFont::Monospace);
        mono.setPixelSize(12);
        setFont(mono);

        const QColor text = theme.textSecondary();
        const QColor surface = theme.bgCanvas().lighter(125);
        const QColor border = theme.borderSubtle();
        setStyleSheet(QStringLiteral(
                          "QLabel {"
                          "  color: %1;"
                          "  background-color: rgba(%2,%3,%4,200);"
                          "  border: 1px solid %5;"
                          "  border-radius: 4px;"
                          "  padding: 3px 8px;"
                          "}")
                          .arg(text.name())
                          .arg(surface.red())
                          .arg(surface.green())
                          .arg(surface.blue())
                          .arg(border.name()));
        setText(QStringLiteral("100%"));
        adjustSize();
    }

    void setZoomPercent(qreal percent)
    {
        setText(QStringLiteral("%1%").arg(qRound(percent)));
        adjustSize();
    }
};

// Hosts the GPU canvas (grid + node layers, sharing one camera) and overlays the
// zoom readout in the bottom-left corner.
class CanvasView : public QWidget {
public:
    explicit CanvasView(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_theme(cutpilot::theme::Theme::Dark)
    {
        qmlRegisterType<CanvasController>("CutPilot.Render", 1, 0, "CanvasController");
        qmlRegisterType<CanvasItem>("CutPilot.Render", 1, 0, "CanvasItem");
        qmlRegisterType<NodeLayerItem>("CutPilot.Render", 1, 0, "NodeLayerItem");

        m_quick = new QQuickWidget(this);
        m_quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_quick->setSource(QUrl(QStringLiteral("qrc:/cutpilot/app/Canvas.qml")));

        QQuickItem *root = m_quick->rootObject();
        if (root) {
            m_controller = root->findChild<CanvasController *>();
            if (auto *layer = root->findChild<NodeLayerItem *>()) {
                m_layer = layer;
                // Setting CUTPILOT_STRESS_NODES to a positive count seeds a wide stress
                // board for the frame-budget check; otherwise the wired starter pair.
                bool ok = false;
                const int stressCount =
                    qEnvironmentVariableIntValue("CUTPILOT_STRESS_NODES", &ok);
                if (ok && stressCount > 0)
                    layer->seedStressBoard(stressCount);
                else
                    layer->seedStarterNode();

                // Dropping a fresh connector on empty canvas asks the chrome for a
                // node palette; a plain menu stands in for the command palette. The
                // queued hop lets the release event finish before the menu blocks.
                QObject::connect(
                    layer, &NodeLayerItem::paletteRequested, this,
                    [this] {
                        const QStringList titles = m_layer->paletteEntryTitles();
                        if (titles.isEmpty()) {
                            m_layer->cancelPalette();
                            return;
                        }
                        QMenu menu(this);
                        for (int i = 0; i < titles.size(); ++i)
                            menu.addAction(titles[i])->setData(i);
                        if (QAction *chosen = menu.exec(QCursor::pos()))
                            m_layer->placePaletteEntry(chosen->data().toInt());
                        else
                            m_layer->cancelPalette();
                    },
                    Qt::QueuedConnection);
            }
        }

        m_readout = new ZoomReadout(m_theme, this);

        if (m_controller) {
            QObject::connect(m_controller, &CanvasController::cameraChanged, this,
                             [this] {
                                 m_readout->setZoomPercent(m_controller->zoomPercent());
                             });
            m_readout->setZoomPercent(m_controller->zoomPercent());
        }
    }

    CanvasController *controller() const { return m_controller; }
    QQuickWidget *quickWidget() const { return m_quick; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_quick->setGeometry(rect());
        const int margin = 12;
        m_readout->move(margin,
                        height() - m_readout->height() - margin);
        m_readout->raise();
    }

private:
    ThemeTable m_theme;
    QQuickWidget *m_quick = nullptr;
    CanvasController *m_controller = nullptr;
    NodeLayerItem *m_layer = nullptr;
    ZoomReadout *m_readout = nullptr;
};

// Continuously pans the camera while timing the gaps between rendered frames, then
// prints the distribution and quits. Driven by CUTPILOT_FRAME_STATS=<frame count>;
// pairs with CUTPILOT_STRESS_NODES to check the frame budget at scale.
void runFrameStats(CanvasView *view, int frameTarget)
{
    // Sweep the camera left, then right, faster than the frame rate so every frame
    // has a pending repaint and the measured cadence is the renderer's own.
    auto *driver = new QTimer(view);
    QObject::connect(driver, &QTimer::timeout, view, [view, tick = 0]() mutable {
        const int phase = (tick / 240) % 2;
        view->controller()->panByPixels(QPointF(phase == 0 ? -12.0 : 12.0, 0.0));
        ++tick;
    });
    driver->start(4);

    struct Capture {
        QElapsedTimer clock;
        std::vector<double> intervalsMs;
        int warmupLeft = 30; // skip startup frames: pipeline builds, font caches
        bool done = false;
    };
    auto capture = std::make_shared<Capture>();
    capture->intervalsMs.reserve(size_t(frameTarget));

    QObject::connect(
        view->quickWidget()->quickWindow(), &QQuickWindow::afterRendering, view,
        [capture, frameTarget, driver] {
            if (capture->done)
                return;
            if (capture->warmupLeft > 0) {
                --capture->warmupLeft;
                capture->clock.start();
                return;
            }
            capture->intervalsMs.push_back(capture->clock.nsecsElapsed() / 1e6);
            capture->clock.restart();
            if (int(capture->intervalsMs.size()) < frameTarget)
                return;

            capture->done = true;
            driver->stop();
            std::vector<double> sorted = capture->intervalsMs;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0.0;
            for (double v : sorted)
                sum += v;
            std::printf("frame intervals: frames=%zu avg=%.2fms median=%.2fms "
                        "p95=%.2fms max=%.2fms\n",
                        sorted.size(), sum / double(sorted.size()),
                        sorted[sorted.size() / 2],
                        sorted[(sorted.size() * 95) / 100], sorted.back());
            std::fflush(stdout);
            QCoreApplication::quit();
        });
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("CutPilot"));
    app.setOrganizationName(QStringLiteral("CutPilot"));

    const ThemeTable theme(cutpilot::theme::Theme::Dark);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("CutPilot"));

    auto *view = new CanvasView(&window);
    view->setStyleSheet(QStringLiteral("background-color: %1;")
                            .arg(theme.bgCanvas().name()));
    window.setCentralWidget(view);
    window.resize(1280, 800);
    window.show();

    // Runtime diagnostics. CUTPILOT_FRAME_STATS=<frames> pans continuously, prints
    // the frame-interval distribution, and exits; CUTPILOT_SCREENSHOT=<path> saves
    // a capture of the window shortly after startup (and exits unless the frame
    // run is still measuring).
    bool statsRequested = false;
    const int statsFrames =
        qEnvironmentVariableIntValue("CUTPILOT_FRAME_STATS", &statsRequested);
    const bool statsActive = statsRequested && statsFrames > 0 && view->controller();
    if (statsActive)
        runFrameStats(view, statsFrames);

    const QString shotPath = qEnvironmentVariable("CUTPILOT_SCREENSHOT");
    if (!shotPath.isEmpty()) {
        QTimer::singleShot(1500, &window, [&window, shotPath, statsActive] {
            window.grab().save(shotPath);
            if (!statsActive)
                QCoreApplication::quit();
        });
    }

    return app.exec();
}
