#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/CanvasItem.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/secrets/KeychainStore.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
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
#include <functional>
#include <memory>
#include <vector>

using cutpilot::ipc::GenerationClient;
using cutpilot::ipc::GenerationCoordinator;
using cutpilot::ipc::SidecarHost;
using cutpilot::render::CanvasController;
using cutpilot::render::CanvasItem;
using cutpilot::render::NodeLayerItem;
using cutpilot::secrets::KeychainStore;
using cutpilot::theme::ThemeTable;

namespace core = cutpilot::core;

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
    NodeLayerItem *layer() const { return m_layer; }
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

// An inline prompt editor floated over a prompt node's body. Committing on
// focus-out or Cmd/Ctrl+Return hands the text back through the layer's
// undoable setter; Escape abandons the edit.
class PromptEditor : public QPlainTextEdit {
public:
    std::function<void(int, const QString &)> onCommit;

    explicit PromptEditor(const ThemeTable &theme, QWidget *parent)
        : QPlainTextEdit(parent)
    {
        hide();
        setStyleSheet(QStringLiteral(
                          "QPlainTextEdit {"
                          "  color: %1;"
                          "  background-color: %2;"
                          "  border: 1px solid %3;"
                          "  border-radius: 4px;"
                          "  padding: 4px;"
                          "}")
                          .arg(theme.textPrimary().name(), theme.nodeBody().name(),
                               theme.selection().name()));
    }

    void open(int nodeId, const QRectF &rect, const QString &text)
    {
        m_nodeId = nodeId;
        setPlainText(text);
        setGeometry(rect.toRect());
        show();
        raise();
        setFocus();
        moveCursor(QTextCursor::End);
    }

protected:
    void focusOutEvent(QFocusEvent *event) override
    {
        commit();
        QPlainTextEdit::focusOutEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_nodeId = -1;
            hide();
            return;
        }
        const bool submit = (event->key() == Qt::Key_Return
                             || event->key() == Qt::Key_Enter)
            && event->modifiers().testAnyFlags(Qt::ControlModifier
                                               | Qt::MetaModifier);
        if (submit) {
            commit();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }

private:
    void commit()
    {
        const int nodeId = m_nodeId;
        m_nodeId = -1;
        if (nodeId != -1 && onCommit)
            onCommit(nodeId, toPlainText());
        hide();
    }

    int m_nodeId = -1;
};

// The chrome surfaces generation asks for: the registry-driven model picker,
// the inline prompt editor, and the add-a-key flow that stores the user's own
// vendor key in the system keychain.
class GenerationChrome : public QObject {
public:
    GenerationChrome(CanvasView *view, GenerationCoordinator *coordinator)
        : QObject(view)
        , m_view(view)
        , m_coordinator(coordinator)
        , m_editor(new PromptEditor(ThemeTable(cutpilot::theme::Theme::Dark), view))
    {
        NodeLayerItem *layer = view->layer();
        m_editor->onCommit = [layer](int nodeId, const QString &text) {
            layer->setNodePrompt(nodeId, text);
        };
    }

    void openPromptEditor(int nodeId)
    {
        NodeLayerItem *layer = m_view->layer();
        const core::Node *node = layer->graph().nodeById(nodeId);
        if (!node)
            return;
        const QRectF rect = layer->nodeBodyScreenRect(nodeId).adjusted(4, 4, -4, -4);
        if (rect.width() < 60 || rect.height() < 40)
            return; // too small to edit in place at this zoom
        m_editor->open(nodeId, rect, node->promptText);
    }

    void showModelPicker(int nodeId)
    {
        const QVector<cutpilot::ipc::ModelInfo> models = m_coordinator->models();
        if (models.isEmpty())
            return; // registry not loaded; the run path reports the details

        QMenu menu(m_view);
        for (const auto &model : models) {
            QString label = model.label;
            if (model.needsKey && !model.hasKey)
                label += QStringLiteral(" — add key");
            menu.addAction(label)->setData(model.id);
        }
        QAction *chosen = menu.exec(QCursor::pos());
        if (!chosen)
            return;
        const QString id = chosen->data().toString();
        for (const auto &model : models) {
            if (model.id == id) {
                m_view->layer()->setNodeModel(nodeId, model.id, model.label);
                return;
            }
        }
    }

    void promptAddKey(int nodeId, const QString &provider)
    {
        bool accepted = false;
        const QString key = QInputDialog::getText(
            m_view, QStringLiteral("Add your %1 API key").arg(provider),
            QStringLiteral("Paste your own %1 API key.\n"
                           "It is stored only in your system keychain and read "
                           "by the generation service.")
                .arg(provider),
            QLineEdit::Password, QString(), &accepted);
        if (!accepted || key.trimmed().isEmpty())
            return;

        if (!KeychainStore::available()
            || !KeychainStore::writeSecret(QStringLiteral("cutpilot"), provider,
                                           key.trimmed())) {
            QMessageBox::warning(
                m_view, QStringLiteral("Key not stored"),
                QStringLiteral("The keychain refused the write. Set the vendor's "
                               "API key environment variable and relaunch."));
            return;
        }
        m_coordinator->refreshModels();
        m_coordinator->runNode(nodeId);
    }

private:
    CanvasView *m_view = nullptr;
    GenerationCoordinator *m_coordinator = nullptr;
    PromptEditor *m_editor = nullptr;
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

    // The generation stack: the service process, its client, and the
    // coordinator that runs nodes against it. The canvas layer raises the
    // gestures; the chrome supplies the picker, editor, and key dialogs.
    SidecarHost host;
    GenerationClient client;
    NodeLayerItem *layer = view->layer();
    GenerationCoordinator *coordinator = nullptr;
    GenerationChrome *chrome = nullptr;
    if (layer) {
        coordinator = new GenerationCoordinator(&layer->graph(), &client, view);
        chrome = new GenerationChrome(view, coordinator);

        QObject::connect(&host, &SidecarHost::ready, coordinator,
                         [&client, coordinator](quint16 port, const QByteArray &token) {
                             client.setEndpoint(port, token);
                             coordinator->serviceBecameReady();
                         });
        QObject::connect(&host, &SidecarHost::failed, coordinator,
                         [coordinator](const QString &reason) {
                             qWarning("%s", qPrintable(reason));
                             coordinator->serviceBecameUnavailable(reason);
                         });

        QObject::connect(layer, &NodeLayerItem::runRequested, coordinator,
                         &GenerationCoordinator::runNode);
        QObject::connect(layer, &NodeLayerItem::stopRequested, coordinator,
                         &GenerationCoordinator::stopNode);
        QObject::connect(layer, &NodeLayerItem::graphMutated, coordinator,
                         &GenerationCoordinator::reconcile);
        QObject::connect(coordinator, &GenerationCoordinator::nodeContentChanged,
                         layer, &NodeLayerItem::refreshNode);
        QObject::connect(coordinator, &GenerationCoordinator::nodeMediaReady, layer,
                         &NodeLayerItem::setNodeMedia);

        QObject::connect(layer, &NodeLayerItem::modelPickerRequested, chrome,
                         [chrome](int nodeId) { chrome->showModelPicker(nodeId); },
                         Qt::QueuedConnection);
        QObject::connect(layer, &NodeLayerItem::promptEditRequested, chrome,
                         [chrome](int nodeId) { chrome->openPromptEditor(nodeId); },
                         Qt::QueuedConnection);
        QObject::connect(coordinator, &GenerationCoordinator::addKeyNeeded, chrome,
                         [chrome](int nodeId, const QString &provider) {
                             chrome->promptAddKey(nodeId, provider);
                         },
                         Qt::QueuedConnection);

        host.start();
    }

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

    // CUTPILOT_AUTORUN=1 runs the board's first generation node as soon as the
    // model registry lands; with CUTPILOT_SCREENSHOT the capture then waits for
    // that run to settle instead of firing on a fixed delay.
    const bool autorun =
        qEnvironmentVariableIntValue("CUTPILOT_AUTORUN") > 0 && coordinator;
    const QString shotPath = qEnvironmentVariable("CUTPILOT_SCREENSHOT");

    if (autorun) {
        QObject::connect(
            coordinator, &GenerationCoordinator::modelsReady, view,
            [layer, coordinator] {
                for (const core::Node &node : layer->graph().nodes()) {
                    if (node.kind == core::NodeKind::Generate) {
                        coordinator->runNode(node.id);
                        return;
                    }
                }
            },
            static_cast<Qt::ConnectionType>(Qt::AutoConnection
                                            | Qt::SingleShotConnection));
    }

    if (!shotPath.isEmpty() && autorun) {
        auto captured = std::make_shared<bool>(false);
        QObject::connect(
            coordinator, &GenerationCoordinator::nodeContentChanged, &window,
            [&window, layer, shotPath, statsActive, captured](int nodeId) {
                const core::Node *node = layer->graph().nodeById(nodeId);
                if (!node || *captured)
                    return;
                const bool settled = node->runState == core::RunState::Done
                    || node->runState == core::RunState::Error
                    || node->runState == core::RunState::NeedsKey;
                if (!settled)
                    return;
                *captured = true;
                QTimer::singleShot(700, &window, [&window, shotPath, statsActive] {
                    window.grab().save(shotPath);
                    if (!statsActive)
                        QCoreApplication::quit();
                });
            });
    } else if (!shotPath.isEmpty()) {
        QTimer::singleShot(1500, &window, [&window, shotPath, statsActive] {
            window.grab().save(shotPath);
            if (!statsActive)
                QCoreApplication::quit();
        });
    }

    return app.exec();
}
