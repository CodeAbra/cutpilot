#include "CanvasCluster.h"
#include "CommandPalette.h"
#include "CompositeInspector.h"
#include "ExportController.h"
#include "QuickPanel.h"
#include "RailPanels.h"
#include "ThemeController.h"
#include "ToolPill.h"
#include "ToolRail.h"
#include "TopBar.h"
#include "WorkflowStore.h"

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/ConvertClient.h"
#include "cutpilot/ipc/GenerationClient.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/ipc/SidecarHost.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/WorkflowJson.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/CanvasItem.h"
#include "cutpilot/render/MinimapItem.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/render/CompositorService.h"
#include "cutpilot/render/PreviewController.h"
#include "cutpilot/render/PreviewItem.h"
#include "cutpilot/secrets/KeychainStore.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QMimeData>
#include <QRadioButton>
#include <QStackedWidget>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QResizeEvent>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using cutpilot::app::CompositeInspector;
using cutpilot::ipc::ConvertClient;
using cutpilot::ipc::GenerationClient;
using cutpilot::ipc::GenerationCoordinator;
using cutpilot::ipc::SidecarHost;
using cutpilot::app::CanvasCluster;
using cutpilot::app::CommandPalette;
using cutpilot::app::PaletteModel;
using cutpilot::app::QuickPanel;
using cutpilot::app::ThemeController;
using cutpilot::app::ToolPill;
using cutpilot::app::ToolRail;
using cutpilot::app::TopBar;
using cutpilot::app::WorkflowStore;
using cutpilot::render::CanvasController;
using cutpilot::render::CanvasItem;
using cutpilot::render::MinimapItem;
using cutpilot::render::CompositorService;
using cutpilot::render::NodeLayerItem;
using cutpilot::render::PreviewController;
using cutpilot::render::PreviewItem;
using cutpilot::secrets::KeychainStore;
using cutpilot::theme::ThemeTable;

namespace core = cutpilot::core;

namespace {

// The pipeline's control strip, floated over the canvas: run everything,
// resume held work or abort while paused, the run-wide spending cap, and the
// live counts/spend readout the coordinator streams.
class RunPanel : public QWidget {
public:
    QPushButton *runAll = nullptr;
    QPushButton *resume = nullptr;
    QPushButton *abort = nullptr;
    QDoubleSpinBox *cap = nullptr;
    QLabel *status = nullptr;

    explicit RunPanel(const ThemeTable &theme, QWidget *parent)
        : QWidget(parent)
    {
        retheme(theme);

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(8, 6, 8, 6);
        row->setSpacing(8);

        runAll = new QPushButton(QStringLiteral("Run all"), this);
        resume = new QPushButton(QStringLiteral("Resume"), this);
        abort = new QPushButton(QStringLiteral("Abort"), this);

        cap = new QDoubleSpinBox(this);
        cap->setPrefix(QStringLiteral("cap $"));
        cap->setDecimals(3);
        cap->setRange(0.0, 1000.0);
        cap->setSingleStep(0.01);
        cap->setSpecialValueText(QStringLiteral("no cap"));
        cap->setToolTip(QStringLiteral("Pause the run before spending past this"));

        status = new QLabel(this);

        row->addWidget(runAll);
        row->addWidget(resume);
        row->addWidget(abort);
        row->addWidget(cap);
        row->addWidget(status);

        showSummary(cutpilot::ipc::RunSummary());
    }

    void retheme(const ThemeTable &theme)
    {
        const QColor surface = theme.bgCanvas().lighter(125);
        setStyleSheet(QStringLiteral(
                          "QWidget { color: %1; }"
                          "QLabel { color: %1; }"
                          "QPushButton {"
                          "  color: %1; background-color: rgba(%2,%3,%4,220);"
                          "  border: 1px solid %5; border-radius: 4px;"
                          "  padding: 3px 10px;"
                          "}"
                          "QPushButton:disabled { color: %6; }"
                          "QDoubleSpinBox {"
                          "  color: %1; background-color: rgba(%2,%3,%4,220);"
                          "  border: 1px solid %5; border-radius: 4px; padding: 2px;"
                          "}")
                          .arg(theme.textPrimary().name())
                          .arg(surface.red())
                          .arg(surface.green())
                          .arg(surface.blue())
                          .arg(theme.borderSubtle().name(),
                               theme.textSecondary().name()));
    }

    void showSummary(const cutpilot::ipc::RunSummary &summary)
    {
        runAll->setEnabled(!summary.active);
        resume->setVisible(summary.active && summary.paused);
        abort->setVisible(summary.active);

        QStringList parts;
        if (summary.total > 0) {
            parts << QStringLiteral("%1%").arg(summary.percent());
            if (summary.running > 0)
                parts << QStringLiteral("%1 running").arg(summary.running);
            if (summary.fresh > 0)
                parts << QStringLiteral("%1 done").arg(summary.fresh);
            if (summary.reused > 0)
                parts << QStringLiteral("%1 reused").arg(summary.reused);
            if (summary.held > 0)
                parts << QStringLiteral("%1 held").arg(summary.held);
            if (summary.failed > 0)
                parts << QStringLiteral("%1 failed").arg(summary.failed);
            QString spend = QStringLiteral("$%1").arg(summary.spentUsd, 0, 'f', 3);
            if (summary.capUsd > 0.0)
                spend += QStringLiteral(" of $%1").arg(summary.capUsd, 0, 'f', 3);
            parts << spend;
            if (summary.paused && !summary.pauseReason.isEmpty())
                parts << summary.pauseReason;
        } else {
            parts << QStringLiteral("Ready");
        }
        status->setText(parts.join(QStringLiteral(" · ")));
        adjustSize();
        if (parentWidget()) {
            move((parentWidget()->width() - width()) / 2, 12);
            raise();
        }
    }
};

// The preview panel floated over the canvas's top-right corner: the GPU
// preview surface, the compare-mode strip, the wipe and overlay controls,
// and the pinned-source readout. Pinning any node summons it; closing hides
// it without dropping the pins.
class PreviewPanel : public QWidget {
public:
    PreviewPanel(const ThemeTable &theme, PreviewController *previews,
                 NodeLayerItem *layer, QWidget *parent)
        : QWidget(parent)
        , m_previews(previews)
        , m_layer(layer)
    {
        retheme(theme);

        auto *column = new QVBoxLayout(this);
        column->setContentsMargins(6, 6, 6, 6);
        column->setSpacing(6);

        auto *header = new QHBoxLayout;
        m_sources = new QLabel(QStringLiteral("Preview"), this);
        auto *close = new QPushButton(QStringLiteral("✕"), this);
        close->setFixedWidth(26);
        header->addWidget(m_sources, 1);
        header->addWidget(close);
        column->addLayout(header);

        m_quick = new QQuickWidget(this);
        m_quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_quick->setSource(QUrl(QStringLiteral("qrc:/cutpilot/app/Preview.qml")));
        m_preview = qobject_cast<PreviewItem *>(m_quick->rootObject());
        if (m_preview) {
            m_preview->setSurroundColor(theme.bgCanvas());
            m_preview->setDividerColor(theme.emphasis());
        }
        column->addWidget(m_quick, 1);

        auto *controls = new QHBoxLayout;
        controls->setSpacing(6);
        m_modes = new QButtonGroup(this);
        QButtonGroup *modes = m_modes;
        const struct {
            const char *label;
            PreviewItem::CompareMode mode;
        } entries[] = {
            { "A", PreviewItem::CompareMode::Single },
            { "Wipe", PreviewItem::CompareMode::Wipe },
            { "Split", PreviewItem::CompareMode::SideBySide },
            { "Diff", PreviewItem::CompareMode::Difference },
            { "Over", PreviewItem::CompareMode::Overlay },
        };
        for (const auto &entry : entries) {
            auto *button = new QPushButton(QString::fromLatin1(entry.label), this);
            button->setCheckable(true);
            modes->addButton(button, int(entry.mode));
            controls->addWidget(button);
        }
        modes->button(0)->setChecked(true);

        m_wipe = new QSlider(Qt::Horizontal, this);
        m_wipe->setRange(0, 100);
        m_wipe->setValue(50);
        m_wipe->setToolTip(QStringLiteral("Wipe position"));
        m_wipe->hide();
        controls->addWidget(m_wipe, 1);

        m_opacity = new QSlider(Qt::Horizontal, this);
        m_opacity->setRange(0, 100);
        m_opacity->setValue(50);
        m_opacity->setToolTip(QStringLiteral("Overlay opacity"));
        m_opacity->hide();
        controls->addWidget(m_opacity, 1);

        controls->addStretch(0);
        m_fit = new QPushButton(QStringLiteral("Fit"), this);
        m_fit->setCheckable(true);
        m_fit->setChecked(true);
        m_fit->setToolTip(QStringLiteral("Fit the view, or show 1:1 texels"));
        controls->addWidget(m_fit);
        column->addLayout(controls);

        connect(close, &QPushButton::clicked, this, &QWidget::hide);
        connect(modes, &QButtonGroup::idClicked, this, [this](int id) {
            const auto mode = PreviewItem::CompareMode(id);
            if (m_preview)
                m_preview->setCompareMode(mode);
            m_wipe->setVisible(mode == PreviewItem::CompareMode::Wipe);
            m_opacity->setVisible(mode == PreviewItem::CompareMode::Overlay);
        });
        connect(m_wipe, &QSlider::valueChanged, this, [this](int value) {
            if (m_preview)
                m_preview->setWipePosition(value / 100.0);
        });
        connect(m_opacity, &QSlider::valueChanged, this, [this](int value) {
            if (m_preview)
                m_preview->setOverlayOpacity(value / 100.0);
        });
        connect(m_fit, &QPushButton::toggled, this, [this](bool fit) {
            if (m_preview)
                m_preview->setFitToView(fit);
        });
        connect(m_previews, &PreviewController::pinsChanged, this, [this] {
            refreshSources();
            if (m_previews->anyPinned()) {
                show();
                raise();
            }
        });

        if (parent)
            parent->installEventFilter(this);
        resize(520, 400);
        hide();
    }

    PreviewItem *previewItem() const { return m_preview; }

    void retheme(const ThemeTable &theme)
    {
        const QColor surface = theme.bgCanvas().lighter(125);
        setStyleSheet(QStringLiteral(
                          "QWidget { color: %1; }"
                          "QLabel { color: %1; background: transparent; "
                          "border: none; }"
                          "QPushButton {"
                          "  color: %1; background-color: rgba(%2,%3,%4,220);"
                          "  border: 1px solid %5; border-radius: 4px;"
                          "  padding: 2px 8px;"
                          "}"
                          "QPushButton:checked { border-color: %6; color: %6; }"
                          "QSlider { background: transparent; }")
                          .arg(theme.textPrimary().name())
                          .arg(surface.red())
                          .arg(surface.green())
                          .arg(surface.blue())
                          .arg(theme.borderSubtle().name(),
                               theme.textPrimary().name()));
        if (m_preview) {
            m_preview->setSurroundColor(theme.bgCanvas());
            m_preview->setDividerColor(theme.emphasis());
        }
    }

    // Drive the compare strip programmatically, keeping the buttons in step.
    void selectMode(PreviewItem::CompareMode mode)
    {
        if (QAbstractButton *button = m_modes->button(int(mode)))
            button->click();
    }

    void refreshSources()
    {
        const auto title = [this](PreviewController::Buffer buffer) {
            const int nodeId = m_previews->pinnedNode(buffer);
            const cutpilot::core::Node *node =
                nodeId != -1 ? m_layer->graph().nodeById(nodeId) : nullptr;
            return node ? node->title : QStringLiteral("—");
        };
        m_sources->setText(QStringLiteral("A: %1 · B: %2")
                               .arg(title(PreviewController::Buffer::A),
                                    title(PreviewController::Buffer::B)));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize)
            reanchor();
        return QWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        reanchor();
    }

private:
    void reanchor()
    {
        if (!parentWidget())
            return;
        const int margin = 12;
        move(parentWidget()->width() - width() - margin, margin);
        raise();
    }

    PreviewController *m_previews = nullptr;
    NodeLayerItem *m_layer = nullptr;
    QQuickWidget *m_quick = nullptr;
    PreviewItem *m_preview = nullptr;
    QLabel *m_sources = nullptr;
    QButtonGroup *m_modes = nullptr;
    QSlider *m_wipe = nullptr;
    QSlider *m_opacity = nullptr;
    QPushButton *m_fit = nullptr;
};

// Hosts the GPU canvas (grid, node, and minimap layers, sharing one camera)
// with the floating chrome — the tool pill, the bottom-left cluster, the rail
// panels, and the run strip — overlaid on top. Media files dropped anywhere
// on it land as still or video nodes at the drop point.
class CanvasView : public QWidget {
public:
    explicit CanvasView(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_theme(cutpilot::theme::Theme::Dark)
    {
        qmlRegisterType<CanvasController>("CutPilot.Render", 1, 0, "CanvasController");
        qmlRegisterType<CanvasItem>("CutPilot.Render", 1, 0, "CanvasItem");
        qmlRegisterType<NodeLayerItem>("CutPilot.Render", 1, 0, "NodeLayerItem");
        qmlRegisterType<MinimapItem>("CutPilot.Render", 1, 0, "MinimapItem");
        qmlRegisterType<PreviewItem>("CutPilot.Render", 1, 0, "PreviewItem");

        m_quick = new QQuickWidget(this);
        m_quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_quick->setSource(QUrl(QStringLiteral("qrc:/cutpilot/app/Canvas.qml")));

        QQuickItem *root = m_quick->rootObject();
        if (root) {
            m_controller = root->findChild<CanvasController *>();
            m_layer = root->findChild<NodeLayerItem *>();
            m_minimap = root->findChild<MinimapItem *>();
            m_grid = root->findChild<CanvasItem *>();
        }

        m_runPanel = new RunPanel(m_theme, this);
        setAcceptDrops(true);
    }

    CanvasController *controller() const { return m_controller; }
    NodeLayerItem *layer() const { return m_layer; }
    MinimapItem *minimap() const { return m_minimap; }
    QQuickWidget *quickWidget() const { return m_quick; }
    RunPanel *runPanel() const { return m_runPanel; }

    // The floating chrome this view keeps anchored: the pill bottom-center,
    // the cluster bottom-left, and any rail panels along the left edge.
    void setFloatingChrome(QWidget *pill, QWidget *cluster,
                           const QVector<QWidget *> &panels)
    {
        m_pill = pill;
        m_cluster = cluster;
        m_panels = panels;
        // Created after this view was shown, so visibility is explicit:
        // the pill and cluster are always up, panels wait for their rail item.
        if (m_pill)
            m_pill->show();
        if (m_cluster)
            m_cluster->show();
        layoutOverlays();
    }

    void layoutOverlays()
    {
        const int margin = 12;
        if (m_pill) {
            m_pill->adjustSize();
            m_pill->move((width() - m_pill->width()) / 2,
                         height() - m_pill->height() - margin);
            m_pill->raise();
        }
        if (m_cluster) {
            m_cluster->adjustSize();
            m_cluster->move(margin, height() - m_cluster->height() - margin);
            m_cluster->raise();
        }
        for (QWidget *panel : m_panels) {
            panel->setFixedHeight(qMax(200, height() - 2 * margin));
            panel->move(margin, margin);
            if (panel->isVisible())
                panel->raise();
        }
        m_runPanel->move((width() - m_runPanel->width()) / 2, margin);
        m_runPanel->raise();
    }

    // Push one theme into every GPU layer this view hosts, and into the run
    // strip it owns.
    void applyCanvasTheme(cutpilot::theme::Theme themeId)
    {
        m_theme.setTheme(themeId);
        setStyleSheet(QStringLiteral("background-color: %1;")
                          .arg(m_theme.bgCanvas().name()));
        m_runPanel->retheme(m_theme);
        if (m_layer)
            m_layer->setTheme(themeId);
        if (m_grid)
            m_grid->setTheme(themeId);
        if (m_minimap)
            m_minimap->setTheme(themeId);
    }

    // The world point under a view-local position, for drops.
    QPointF worldAtPos(const QPointF &localPos) const
    {
        if (!m_controller)
            return localPos;
        const qreal dpr = devicePixelRatioF();
        return m_controller->worldFromScreen(localPos * dpr, dpr);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_quick->setGeometry(rect());
        layoutOverlays();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!m_layer || !event->mimeData()->hasUrls())
            return;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()
                && cutpilot::app::AssetsPanel::isMediaFile(url.toLocalFile())) {
                event->acceptProposedAction();
                return;
            }
        }
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!m_layer)
            return;
        int placed = 0;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (!url.isLocalFile()
                || !cutpilot::app::AssetsPanel::isMediaFile(url.toLocalFile()))
                continue;
            // Fan multiple files out so they never stack invisibly.
            const QPointF offset(placed * 40.0, placed * 40.0);
            m_layer->placePrototypeAt(
                cutpilot::app::AssetsPanel::prototypeForFile(url.toLocalFile()),
                worldAtPos(event->position()) + offset);
            ++placed;
        }
        if (placed > 0)
            event->acceptProposedAction();
    }

private:
    ThemeTable m_theme;
    QQuickWidget *m_quick = nullptr;
    CanvasController *m_controller = nullptr;
    NodeLayerItem *m_layer = nullptr;
    MinimapItem *m_minimap = nullptr;
    CanvasItem *m_grid = nullptr;
    RunPanel *m_runPanel = nullptr;
    QWidget *m_pill = nullptr;
    QWidget *m_cluster = nullptr;
    QVector<QWidget *> m_panels;
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
        retheme(theme);
    }

    void retheme(const ThemeTable &theme)
    {
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

// A video node's transport, floated over the canvas's bottom edge: play or
// pause, the scrub bar, and the position readout. Scrubbing asks the media
// stack to seek and returns immediately; the sought frame arrives on its own
// and flows into the card and the preview.
class VideoTransport : public QWidget {
public:
    VideoTransport(const ThemeTable &theme, NodeLayerItem *layer,
                   CompositorService *media, QWidget *parent)
        : QWidget(parent)
        , m_layer(layer)
        , m_media(media)
    {
        retheme(theme);

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(8, 6, 8, 6);
        row->setSpacing(8);

        m_title = new QLabel(this);
        m_play = new QPushButton(QStringLiteral("▶"), this);
        m_play->setFixedWidth(34);
        m_scrub = new QSlider(Qt::Horizontal, this);
        m_scrub->setRange(0, 1000);
        m_time = new QLabel(this);
        auto *close = new QPushButton(QStringLiteral("✕"), this);
        close->setFixedWidth(26);

        row->addWidget(m_title);
        row->addWidget(m_play);
        row->addWidget(m_scrub, 1);
        row->addWidget(m_time);
        row->addWidget(close);

        connect(close, &QPushButton::clicked, this, &QWidget::hide);
        connect(m_play, &QPushButton::clicked, this, [this] {
            m_media->setVideoPlaying(m_nodeId, !m_media->videoPlaying(m_nodeId));
        });
        connect(m_scrub, &QSlider::sliderMoved, this, [this](int value) {
            m_media->scrubVideo(m_nodeId, value / 1000.0);
        });
        connect(m_media, &CompositorService::videoStateChanged, this,
                [this](int nodeId) {
                    if (nodeId == m_nodeId && isVisible())
                        readState();
                });
        connect(layer, &NodeLayerItem::graphMutated, this, [this] {
            if (isVisible() && m_nodeId != -1
                && !m_layer->graph().nodeById(m_nodeId))
                hide();
        });

        if (parent)
            parent->installEventFilter(this);
        setFixedWidth(520);
        hide();
    }

    void retheme(const ThemeTable &theme)
    {
        const QColor surface = theme.bgCanvas().lighter(125);
        setStyleSheet(QStringLiteral(
                          "QWidget { color: %1; }"
                          "QLabel { color: %1; background: transparent; "
                          "border: none; }"
                          "QPushButton {"
                          "  color: %1; background-color: rgba(%2,%3,%4,220);"
                          "  border: 1px solid %5; border-radius: 4px;"
                          "  padding: 2px 10px;"
                          "}"
                          "QSlider { background: transparent; }")
                          .arg(theme.textPrimary().name())
                          .arg(surface.red())
                          .arg(surface.green())
                          .arg(surface.blue())
                          .arg(theme.borderSubtle().name()));
    }

    void openFor(int nodeId)
    {
        const core::Node *node = m_layer->graph().nodeById(nodeId);
        if (!node || node->kind != core::NodeKind::Video)
            return;
        m_nodeId = nodeId;
        m_title->setText(node->title);
        readState();
        show();
        raise();
        reanchor();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize)
            reanchor();
        return QWidget::eventFilter(watched, event);
    }

private:
    void reanchor()
    {
        if (!parentWidget())
            return;
        move((parentWidget()->width() - width()) / 2,
             parentWidget()->height() - height() - 52);
        raise();
    }

    void readState()
    {
        const qint64 duration = m_media->videoDurationMs(m_nodeId);
        const qint64 position = m_media->videoPositionMs(m_nodeId);
        m_play->setText(m_media->videoPlaying(m_nodeId) ? QStringLiteral("⏸")
                                                        : QStringLiteral("▶"));
        if (!m_scrub->isSliderDown() && duration > 0)
            m_scrub->setValue(int(position * 1000 / duration));
        const auto stamp = [](qint64 ms) {
            return QStringLiteral("%1:%2.%3")
                .arg(ms / 60000)
                .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
                .arg((ms / 100) % 10);
        };
        m_time->setText(
            QStringLiteral("%1 / %2").arg(stamp(position), stamp(duration)));
    }

    NodeLayerItem *m_layer = nullptr;
    CompositorService *m_media = nullptr;
    QLabel *m_title = nullptr;
    QPushButton *m_play = nullptr;
    QSlider *m_scrub = nullptr;
    QLabel *m_time = nullptr;
    int m_nodeId = -1;
};

// The chrome surfaces the canvas asks for: the registry-driven model picker,
// the inline prompt editor, the add-a-key flow that stores the user's own
// vendor key in the system keychain, and the per-node menu carrying run and
// preview-pin actions.
class GenerationChrome : public QObject {
public:
    GenerationChrome(const ThemeTable &theme, CanvasView *view,
                     GenerationCoordinator *coordinator,
                     PreviewController *previews)
        : QObject(view)
        , m_view(view)
        , m_coordinator(coordinator)
        , m_previews(previews)
        , m_editor(new PromptEditor(theme, view))
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

    void showNodeMenu(int nodeId)
    {
        NodeLayerItem *layer = m_view->layer();
        const core::Node *node = layer->graph().nodeById(nodeId);
        if (!node)
            return;

        QMenu menu(m_view);
        QAction *runHere = nullptr;
        QAction *rerun = nullptr;
        if (node->kind == core::NodeKind::Generate) {
            runHere = menu.addAction(QStringLiteral("Run to here"));
            rerun = menu.addAction(QStringLiteral("Re-run ignoring cache"));
        }

        QAction *pinA = nullptr;
        QAction *pinB = nullptr;
        QAction *unpin = nullptr;
        if (m_previews && core::producesImage(node->kind)) {
            if (!menu.isEmpty())
                menu.addSeparator();
            pinA = menu.addAction(QStringLiteral("Preview in A"));
            pinB = menu.addAction(QStringLiteral("Preview in B"));
            const bool pinned =
                m_previews->pinnedNode(PreviewController::Buffer::A) == nodeId
                || m_previews->pinnedNode(PreviewController::Buffer::B) == nodeId;
            if (pinned)
                unpin = menu.addAction(QStringLiteral("Unpin from preview"));
        }
        if (menu.isEmpty())
            return;

        QAction *chosen = menu.exec(QCursor::pos());
        if (!chosen)
            return;
        if (chosen == runHere) {
            m_coordinator->runTo(nodeId);
        } else if (chosen == rerun) {
            m_coordinator->rerunNode(nodeId);
        } else if (chosen == pinA) {
            m_previews->pin(PreviewController::Buffer::A, nodeId);
        } else if (chosen == pinB) {
            m_previews->pin(PreviewController::Buffer::B, nodeId);
        } else if (chosen == unpin) {
            if (m_previews->pinnedNode(PreviewController::Buffer::A) == nodeId)
                m_previews->unpin(PreviewController::Buffer::A);
            if (m_previews->pinnedNode(PreviewController::Buffer::B) == nodeId)
                m_previews->unpin(PreviewController::Buffer::B);
        }
    }

    void editGateLimit(int nodeId)
    {
        NodeLayerItem *layer = m_view->layer();
        const core::Node *node = layer->graph().nodeById(nodeId);
        if (!node || node->kind != core::NodeKind::CostGate)
            return;
        bool accepted = false;
        const double limit = QInputDialog::getDouble(
            m_view, QStringLiteral("Cost gate limit"),
            QStringLiteral("Pause this gate's branch once the run has spent"),
            node->gateLimitUsd, 0.0, 1000.0, 3, &accepted,
            Qt::WindowFlags(), 0.01);
        if (accepted)
            layer->setGateLimit(nodeId, limit);
    }

    // With a node in hand the stored key immediately reruns it; from the
    // settings surface (no node) it only lands in the keychain.
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
        if (nodeId != -1)
            m_coordinator->runNode(nodeId);
    }

    void retheme(const ThemeTable &theme) { m_editor->retheme(theme); }

private:
    CanvasView *m_view = nullptr;
    GenerationCoordinator *m_coordinator = nullptr;
    PreviewController *m_previews = nullptr;
    PromptEditor *m_editor = nullptr;
};

// An honest surface for a mode whose work area is not built yet: the switch
// itself is real, the page says exactly what it is.
class ModePage : public QWidget {
public:
    ModePage(const QString &name, const ThemeTable &theme,
             QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_StyledBackground, true);
        auto *column = new QVBoxLayout(this);
        column->addStretch(2);
        m_title = new QLabel(name, this);
        m_title->setAlignment(Qt::AlignCenter);
        m_hint = new QLabel(
            QStringLiteral("This mode's work area isn't built yet.\n"
                           "Node is the live screen."),
            this);
        m_hint->setAlignment(Qt::AlignCenter);
        column->addWidget(m_title);
        column->addWidget(m_hint);
        column->addStretch(3);
        retheme(theme);
    }

    void retheme(const ThemeTable &theme)
    {
        setStyleSheet(QStringLiteral("background-color: %1;")
                          .arg(theme.bgCanvas().name()));
        m_title->setStyleSheet(
            QStringLiteral("color: %1; font-size: 20px; font-weight: 600;")
                .arg(theme.textPrimary().name()));
        m_hint->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;")
                                  .arg(theme.textSecondary().name()));
    }

private:
    QLabel *m_title = nullptr;
    QLabel *m_hint = nullptr;
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

    ThemeController themes;
    const ThemeTable &theme = themes.table();

    // Declared before the window so they outlive the coordinator and chrome
    // parented into its widget tree, which hold pointers to them.
    SidecarHost host;
    GenerationClient client;
    SidecarHost convertHost{ QStringLiteral("sidecars/convert"),
                             QStringLiteral("convert") };
    ConvertClient convertClient;

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("CutPilot"));

    // The frame: the top chrome over a body of icon rail + mode pages, with
    // the Node screen's canvas as the live page.
    auto *frame = new QWidget(&window);
    auto *frameColumn = new QVBoxLayout(frame);
    frameColumn->setContentsMargins(0, 0, 0, 0);
    frameColumn->setSpacing(0);

    auto *topBar = new TopBar(theme, frame);
    frameColumn->addWidget(topBar);

    auto *body = new QWidget(frame);
    auto *bodyRow = new QHBoxLayout(body);
    bodyRow->setContentsMargins(0, 0, 0, 0);
    bodyRow->setSpacing(0);
    auto *rail = new ToolRail(theme, body);
    auto *modeStack = new QStackedWidget(body);
    bodyRow->addWidget(rail);
    bodyRow->addWidget(modeStack, 1);
    frameColumn->addWidget(body, 1);

    auto *view = new CanvasView(modeStack);
    const int nodeModeIndex = TopBar::modeNames().indexOf(QStringLiteral("Node"));
    QVector<ModePage *> modePages;
    for (int i = 0; i < TopBar::modeNames().size(); ++i) {
        if (i == nodeModeIndex) {
            modeStack->addWidget(view);
        } else {
            auto *page = new ModePage(TopBar::modeNames()[i], theme, modeStack);
            modePages.push_back(page);
            modeStack->addWidget(page);
        }
    }
    modeStack->setCurrentIndex(nodeModeIndex);
    topBar->setActiveMode(nodeModeIndex);
    QObject::connect(topBar, &TopBar::modeSelected, modeStack,
                     &QStackedWidget::setCurrentIndex);

    view->applyCanvasTheme(themes.theme());
    window.setCentralWidget(frame);
    window.resize(1440, 900);
    window.show();

    // The generation stack: the service process, its client, and the
    // coordinator that runs nodes against it. The canvas layer raises the
    // gestures; the chrome supplies the picker, editor, and key dialogs; the
    // preview controller keeps the pinned buffers in step with the graph.
    NodeLayerItem *layer = view->layer();
    GenerationCoordinator *coordinator = nullptr;
    GenerationChrome *chrome = nullptr;
    PreviewController *previews = nullptr;
    PreviewPanel *previewPanel = nullptr;
    ExportController *exporter = nullptr;
    WorkflowStore *store = nullptr;
    if (layer) {
        // CUTPILOT_STRESS_NODES seeds a wide stress board for the frame-budget
        // check; CUTPILOT_COMPOSITE_BOARD seeds the local compositing chain.
        // Both are throwaway demo boards, so autosave stays out of their way.
        // Otherwise the stored workflow loads — its saved generation results
        // decode back into the cards — or a fresh board starts wired.
        bool stressRequested = false;
        const int stressCount =
            qEnvironmentVariableIntValue("CUTPILOT_STRESS_NODES", &stressRequested);
        const bool demoBoard = (stressRequested && stressCount > 0)
            || qEnvironmentVariableIntValue("CUTPILOT_COMPOSITE_BOARD") > 0;
        if (stressRequested && stressCount > 0) {
            layer->seedStressBoard(stressCount);
        } else if (qEnvironmentVariableIntValue("CUTPILOT_COMPOSITE_BOARD") > 0) {
            layer->seedCompositeBoard();
        } else {
            store = new WorkflowStore(&layer->graph(), view);
            if (store->load()) {
                layer->graphReloaded();
                for (const core::Node &node : layer->graph().nodes()) {
                    if (node.resultPath.isEmpty())
                        continue;
                    const QImage media(node.resultPath);
                    if (!media.isNull())
                        layer->setNodeMedia(node.id, media);
                }
            } else {
                layer->seedStarterNode();
                store->scheduleSave();
            }
            QObject::connect(layer, &NodeLayerItem::graphMutated, store,
                             &WorkflowStore::scheduleSave);
            // Quitting inside the debounce window must not lose the edit.
            QObject::connect(&app, &QCoreApplication::aboutToQuit, store,
                             &WorkflowStore::flushPendingSave);
        }


        coordinator = new GenerationCoordinator(&layer->graph(), &client, view);
        if (store) {
            // Run results land outside the command stack; a finished result
            // still belongs in the document, or a reopened board forgets it.
            QObject::connect(coordinator, &GenerationCoordinator::nodeMediaReady,
                             store, [store] { store->scheduleSave(); });
        }
        previews = new PreviewController(view);
        previews->setLayer(layer);
        previewPanel = new PreviewPanel(theme, previews, layer, view);
        previews->setPreviewItem(previewPanel->previewItem());
        chrome = new GenerationChrome(theme, view, coordinator, previews);

        // Card pixels off the scrub path: still decodes and composite
        // thumbnails land debounced; the preview follows fresh stills.
        auto *media = new CompositorService(view);
        media->setLayer(layer);
        QObject::connect(media, &CompositorService::mediaUpdated, previews,
                         &PreviewController::refresh);
        QObject::connect(coordinator, &GenerationCoordinator::nodeMediaReady,
                         media, &CompositorService::scheduleRefresh);

        // The compositing inspector, the media file picker, and the video
        // transport.
        auto *inspector = new CompositeInspector(theme, layer, previews, view);
        auto *transport = new VideoTransport(theme, layer, media, view);
        QObject::connect(
            layer, &NodeLayerItem::compositeEditRequested, inspector,
            [inspector](int nodeId) { inspector->openFor(nodeId); },
            Qt::QueuedConnection);
        QObject::connect(
            layer, &NodeLayerItem::videoTransportRequested, transport,
            [transport](int nodeId) { transport->openFor(nodeId); },
            Qt::QueuedConnection);
        QObject::connect(
            layer, &NodeLayerItem::mediaPickRequested, view,
            [view, layer](int nodeId) {
                const core::Node *node = layer->graph().nodeById(nodeId);
                if (!node)
                    return;
                const bool video = node->kind == core::NodeKind::Video;
                const QString path = QFileDialog::getOpenFileName(
                    view,
                    video ? QStringLiteral("Choose a video")
                          : QStringLiteral("Choose an image"),
                    QString(),
                    video
                        ? QStringLiteral("Videos (*.mp4 *.mov *.m4v *.webm)")
                        : QStringLiteral("Images (*.png *.jpg *.jpeg *.webp "
                                         "*.bmp *.tif *.tiff)"));
                if (!path.isEmpty())
                    layer->setNodeMediaPath(nodeId, path);
            },
            Qt::QueuedConnection);

        QObject::connect(&host, &SidecarHost::ready, coordinator,
                         [clientPtr = &client, coordinator](quint16 port,
                                                            const QByteArray &token) {
                             clientPtr->setEndpoint(port, token);
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
        // The preview follows the run. One connection per signal stores the
        // layer's state and then refreshes, so the refresh can never read
        // media the layer has not stored yet — the ordering is structural,
        // not an artifact of connection registration order.
        QObject::connect(coordinator, &GenerationCoordinator::nodeContentChanged,
                         layer, [layer, previews](int nodeId) {
                             layer->refreshNode(nodeId);
                             previews->refresh();
                         });
        QObject::connect(coordinator, &GenerationCoordinator::nodeMediaReady,
                         layer,
                         [layer, previews](int nodeId, const QImage &image) {
                             layer->setNodeMedia(nodeId, image);
                             previews->refresh();
                         });

        QObject::connect(layer, &NodeLayerItem::modelPickerRequested, chrome,
                         [chrome](int nodeId) { chrome->showModelPicker(nodeId); },
                         Qt::QueuedConnection);
        QObject::connect(layer, &NodeLayerItem::promptEditRequested, chrome,
                         [chrome](int nodeId) { chrome->openPromptEditor(nodeId); },
                         Qt::QueuedConnection);
        QObject::connect(layer, &NodeLayerItem::nodeMenuRequested, chrome,
                         [chrome](int nodeId) { chrome->showNodeMenu(nodeId); },
                         Qt::QueuedConnection);
        QObject::connect(layer, &NodeLayerItem::gateLimitEditRequested, chrome,
                         [chrome](int nodeId) { chrome->editGateLimit(nodeId); },
                         Qt::QueuedConnection);
        QObject::connect(coordinator, &GenerationCoordinator::addKeyNeeded, chrome,
                         [chrome](int nodeId, const QString &provider) {
                             chrome->promptAddKey(nodeId, provider);
                         },
                         Qt::QueuedConnection);

        // The run panel drives whole-graph runs and reflects the live
        // summary; a refused run explains itself on the same strip.
        RunPanel *panel = view->runPanel();
        QObject::connect(panel->runAll, &QPushButton::clicked, coordinator,
                         &GenerationCoordinator::runGraph);
        QObject::connect(panel->resume, &QPushButton::clicked, coordinator,
                         &GenerationCoordinator::resumeRun);
        QObject::connect(panel->abort, &QPushButton::clicked, coordinator,
                         &GenerationCoordinator::abortRun);
        QObject::connect(panel->cap, &QDoubleSpinBox::valueChanged, coordinator,
                         &GenerationCoordinator::setRunCapUsd);
        QObject::connect(coordinator, &GenerationCoordinator::runSummaryChanged,
                         panel,
                         [panel](const cutpilot::ipc::RunSummary &summary) {
                             panel->showSummary(summary);
                         });
        QObject::connect(coordinator, &GenerationCoordinator::runRefused, panel,
                         [panel](const QString &reason) {
                             panel->status->setText(reason);
                             panel->adjustSize();
                         });

        // The way out: the export bundle, the project-model landing, the
        // ComfyUI import, and the Resolve hand-off, reached through the
        // File menu and reporting on the same status strip.
        exporter = new ExportController(layer, &convertClient, media, view);
        QObject::connect(exporter, &ExportController::statusChanged, panel,
                         [panel](const QString &message) {
                             panel->status->setText(message);
                             panel->adjustSize();
                         });

        QMenu *fileMenu =
            window.menuBar()->addMenu(QStringLiteral("File"));
        QObject::connect(
            fileMenu->addAction(QStringLiteral("Export Timeline…")),
            &QAction::triggered, exporter,
            [exporter, viewPtr = view] {
                exporter->exportTimelineInteractive(viewPtr);
            });
        QObject::connect(
            fileMenu->addAction(QStringLiteral("Add Results to Timeline")),
            &QAction::triggered, exporter,
            [exporter] { exporter->addResultsToTimeline(); });
        QObject::connect(
            fileMenu->addAction(
                QStringLiteral("Import ComfyUI Workflow…")),
            &QAction::triggered, exporter,
            [exporter, viewPtr = view] {
                exporter->importComfyInteractive(viewPtr);
            });
        QObject::connect(
            fileMenu->addAction(QStringLiteral("Send to DaVinci Resolve")),
            &QAction::triggered, exporter,
            [exporter] { exporter->sendToResolve(); });

        QObject::connect(&convertHost, &SidecarHost::ready, &convertClient,
                         [clientPtr = &convertClient](quint16 port,
                                                      const QByteArray &token) {
                             clientPtr->setEndpoint(port, token);
                         });
        QObject::connect(&convertHost, &SidecarHost::failed, view,
                         [panel](const QString &reason) {
                             qWarning("%s", qPrintable(reason));
                             panel->status->setText(reason);
                             panel->adjustSize();
                         });
        convertHost.start();

        // ------- The screen frame: palette, rail panels, pill, cluster -------

        const auto viewCenterWorld = [view] {
            return view->worldAtPos(
                QPointF(view->width() / 2.0, view->height() / 2.0));
        };
        const auto modelEntries = [coordinator] {
            QVector<PaletteModel::ModelEntry> entries;
            for (const auto &model : coordinator->models())
                entries.push_back(
                    { model.id, model.label, model.provider, model.hasKey });
            return entries;
        };

        // The command palette serves every entry point. A summons remembers
        // where the pick should land; connector drops open the typed offers.
        auto *palette = new CommandPalette(theme, view);
        auto pendingWorld = std::make_shared<QPointF>();
        auto pendingWorldValid = std::make_shared<bool>(false);
        const auto openPaletteAt = [palette, pendingWorld,
                                    pendingWorldValid](const QPointF &world) {
            *pendingWorld = world;
            *pendingWorldValid = true;
            palette->open();
        };
        QObject::connect(
            layer, &NodeLayerItem::paletteInvoked, palette,
            [openPaletteAt](QPointF world) { openPaletteAt(world); },
            Qt::QueuedConnection);
        QObject::connect(
            layer, &NodeLayerItem::paletteRequested, palette,
            [palette, layer] {
                const QStringList titles = layer->paletteEntryTitles();
                if (titles.isEmpty()) {
                    layer->cancelPalette();
                    return;
                }
                palette->openOffers(titles);
            },
            Qt::QueuedConnection);
        QObject::connect(palette, &CommandPalette::offerChosen, layer,
                         [layer](const QString &title) {
                             const int index =
                                 layer->paletteEntryTitles().indexOf(title);
                             if (index >= 0)
                                 layer->placePaletteEntry(index);
                             else
                                 layer->cancelPalette();
                         });
        QObject::connect(palette, &CommandPalette::prototypeChosen, layer,
                         [layer, viewCenterWorld, pendingWorld,
                          pendingWorldValid](const core::Node &prototype) {
                             const QPointF world = *pendingWorldValid
                                 ? *pendingWorld
                                 : viewCenterWorld();
                             *pendingWorldValid = false;
                             layer->placePrototypeAt(prototype, world);
                         });
        QObject::connect(palette, &CommandPalette::dismissed, layer,
                         [layer] { layer->cancelPalette(); });
        QObject::connect(coordinator, &GenerationCoordinator::modelsReady,
                         palette, [palette, modelEntries] {
                             palette->model().setModels(modelEntries());
                         });

        // The tool pill drives the canvas tools; upload and quick land here.
        auto *pill = new ToolPill(theme, layer, view);
        QObject::connect(pill, &ToolPill::paletteRequested, palette,
                         [openPaletteAt, viewCenterWorld] {
                             openPaletteAt(viewCenterWorld());
                         });
        QObject::connect(
            pill, &ToolPill::uploadRequested, view,
            [view, layer, viewCenterWorld] {
                const QString path = QFileDialog::getOpenFileName(
                    view, QStringLiteral("Bring in a file"), QString(),
                    QStringLiteral("Media (*.png *.jpg *.jpeg *.webp *.bmp "
                                   "*.tif *.tiff *.mp4 *.mov *.m4v *.webm)"));
                if (!path.isEmpty())
                    layer->placePrototypeAt(
                        cutpilot::app::AssetsPanel::prototypeForFile(path),
                        viewCenterWorld());
            });

        // The bottom-left cluster: minimap toggle, history, zoom.
        auto *cluster =
            new CanvasCluster(theme, layer, view->controller(), view);
        QObject::connect(cluster, &CanvasCluster::minimapToggled, view,
                         [view](bool visible) {
                             if (auto *minimap = view->minimap())
                                 minimap->setVisible(visible);
                         });
        QObject::connect(layer, &NodeLayerItem::minimapToggleRequested, cluster,
                         [cluster] {
                             cluster->setMinimapVisible(
                                 !cluster->minimapVisible());
                         });
        QObject::connect(layer, &NodeLayerItem::fitAllRequested, cluster,
                         [cluster] { cluster->fitAll(); });

        // The rail's panels operate on the live board.
        const QString templatesDirectory =
            (store ? store->directory() : WorkflowStore::defaultDirectory())
            + QStringLiteral("/templates");
        auto *contentPanel = new cutpilot::app::ContentPanel(theme, layer, view);
        auto *searchPanel =
            new cutpilot::app::SearchPanel(theme, layer, modelEntries, view);
        auto *assetsPanel = new cutpilot::app::AssetsPanel(theme, view);
        auto *builderPanel = new cutpilot::app::BuilderPanel(
            theme, layer, templatesDirectory, view);
        view->setFloatingChrome(pill, cluster,
                                { contentPanel, searchPanel, assetsPanel,
                                  builderPanel });

        const auto panelFor =
            [contentPanel, searchPanel, assetsPanel,
             builderPanel](ToolRail::Item item) -> cutpilot::app::RailPanel * {
            switch (item) {
            case ToolRail::Item::Content:
                return contentPanel;
            case ToolRail::Item::Search:
                return searchPanel;
            case ToolRail::Item::Assets:
                return assetsPanel;
            case ToolRail::Item::Builder:
                return builderPanel;
            }
            return nullptr;
        };
        QObject::connect(
            rail, &ToolRail::panelToggled, view,
            [view, panelFor, contentPanel](ToolRail::Item item, bool open) {
                cutpilot::app::RailPanel *panel = panelFor(item);
                if (!panel)
                    return;
                if (open) {
                    if (panel == contentPanel)
                        contentPanel->refresh();
                    panel->show();
                    view->layoutOverlays();
                } else {
                    panel->hide();
                }
            });
        for (cutpilot::app::RailPanel *panel :
             { static_cast<cutpilot::app::RailPanel *>(contentPanel),
               static_cast<cutpilot::app::RailPanel *>(searchPanel),
               static_cast<cutpilot::app::RailPanel *>(assetsPanel),
               static_cast<cutpilot::app::RailPanel *>(builderPanel) }) {
            QObject::connect(panel, &cutpilot::app::RailPanel::closeRequested,
                             rail, &ToolRail::closeAll);
        }

        const auto jumpToNode = [view, layer](int nodeId) {
            const core::Node *node = layer->graph().nodeById(nodeId);
            if (!node)
                return;
            const qreal dpr = view->devicePixelRatioF();
            view->controller()->centerOnWorld(
                node->worldRect().center(),
                QSizeF(layer->width(), layer->height()) * dpr, dpr);
            layer->graph().selectOnly(nodeId);
            layer->refreshNode(nodeId);
        };
        QObject::connect(contentPanel, &cutpilot::app::ContentPanel::nodeActivated,
                         view, jumpToNode);
        QObject::connect(searchPanel, &cutpilot::app::SearchPanel::nodeActivated,
                         view, jumpToNode);
        QObject::connect(searchPanel,
                         &cutpilot::app::SearchPanel::prototypeChosen, layer,
                         [layer, viewCenterWorld](const core::Node &prototype) {
                             layer->placePrototypeAt(prototype,
                                                     viewCenterWorld());
                         });
        QObject::connect(assetsPanel, &cutpilot::app::AssetsPanel::assetChosen,
                         layer, [layer, viewCenterWorld](const QString &path) {
                             layer->placePrototypeAt(
                                 cutpilot::app::AssetsPanel::prototypeForFile(
                                     path),
                                 viewCenterWorld());
                         });
        QObject::connect(
            builderPanel, &cutpilot::app::BuilderPanel::templateChosen, layer,
            [layer, viewCenterWorld](const QVector<core::Node> &prototypes,
                                     const QVector<core::Connection> &wires) {
                layer->placeSubgraphAt(prototypes, wires, viewCenterWorld());
            });
        QObject::connect(rail, &ToolRail::nodesRequested, palette,
                         [openPaletteAt, viewCenterWorld] {
                             openPaletteAt(viewCenterWorld());
                         });
        QObject::connect(
            rail, &ToolRail::helpRequested, &window, [&window] {
                QMessageBox::information(
                    &window, QStringLiteral("Shortcuts"),
                    QStringLiteral(
                        "Tab — command palette\n"
                        "Double-click empty canvas — command palette\n"
                        "Space-drag / middle-drag — pan\n"
                        "Ctrl+scroll — zoom toward cursor\n"
                        "Ctrl+0 — reset zoom · F — fit all · M — minimap\n"
                        "Delete — remove selection\n"
                        "Ctrl+Z / Ctrl+Shift+Z — undo / redo\n"
                        "Esc — cancel a wire, tool, or selection"));
            });

        // ------- Quick Mode: the one-prompt surface over a real node -------

        // While the quick surface is up the screen reduces to it: the side
        // panels close and the cluster and minimap step aside. The pill (with
        // the toggle itself) and the top bar stay. Everything the surface
        // builds is an ordinary node, so leaving it just reveals the board.
        auto *quick = new QuickPanel(theme, layer, coordinator, view);
        const auto reduceChrome = [view, cluster, rail](bool reduced) {
            cluster->setVisible(!reduced);
            if (auto *minimap = view->minimap())
                minimap->setVisible(!reduced && cluster->minimapVisible());
            if (reduced)
                rail->closeAll();
        };
        QObject::connect(pill, &ToolPill::quickModeToggled, quick,
                         [quick, viewCenterWorld, reduceChrome](bool active) {
                             if (active) {
                                 reduceChrome(true);
                                 quick->openAt(viewCenterWorld());
                             } else {
                                 quick->dismiss();
                                 reduceChrome(false);
                             }
                         });
        QObject::connect(quick, &QuickPanel::dismissed, pill,
                         [pill] { pill->setQuickModeActive(false); });
        QObject::connect(quick, &QuickPanel::nodeFocusRequested, view,
                         jumpToNode);
        QObject::connect(quick, &QuickPanel::addKeyRequested, chrome,
                         [chrome](int nodeId, const QString &provider) {
                             chrome->promptAddKey(nodeId, provider);
                         });

        // A scripted pass over the same quick surface: toggle it on through
        // the pill, land a prompt in its field, press its run control — for
        // demos and evidence captures on machines without pointer automation.
        if (qEnvironmentVariableIntValue("CUTPILOT_QUICK_DEMO") > 0) {
            QObject::connect(
                coordinator, &GenerationCoordinator::modelsReady, quick,
                [pill, quick] {
                    pill->setQuickModeActive(true);
                    if (auto *prompt = quick->findChild<QPlainTextEdit *>(
                            QStringLiteral("quickPrompt")))
                        prompt->setPlainText(QStringLiteral(
                            "A lighthouse on a basalt cliff at dusk, warm "
                            "window light, drifting fog"));
                    if (auto *run = quick->findChild<QPushButton *>(
                            QStringLiteral("quickRun")))
                        run->click();
                },
                static_cast<Qt::ConnectionType>(Qt::AutoConnection
                                                | Qt::SingleShotConnection));
        }

        // ------- Top bar: name, autosave state, share, settings, account ---

        std::function<void()> showSyncState;
        if (store) {
            topBar->setWorkflowName(store->name());
            window.setWindowTitle(
                QStringLiteral("%1 — CutPilot").arg(store->name()));
            QObject::connect(topBar, &TopBar::workflowNameCommitted, store,
                             &WorkflowStore::setName);
            QObject::connect(store, &WorkflowStore::nameChanged, topBar,
                             [topBar, store, &window] {
                                 topBar->setWorkflowName(store->name());
                                 window.setWindowTitle(
                                     QStringLiteral("%1 — CutPilot")
                                         .arg(store->name()));
                             });
            showSyncState = [topBar, store, &themes] {
                const ThemeTable &table = themes.table();
                switch (store->state()) {
                case WorkflowStore::State::Pending:
                    topBar->setSyncState(table.statusRunning(),
                                         QStringLiteral("Saving…"));
                    break;
                case WorkflowStore::State::Saved:
                    topBar->setSyncState(
                        table.statusDone(),
                        QStringLiteral("Saved %1").arg(
                            store->savedAt().toString(
                                QStringLiteral("HH:mm"))));
                    break;
                case WorkflowStore::State::Failed:
                    topBar->setSyncState(table.statusError(),
                                         QStringLiteral("Save failed"));
                    break;
                case WorkflowStore::State::Idle:
                    topBar->setSyncState(table.textSecondary(),
                                         QStringLiteral("Autosave on"));
                    break;
                }
            };
            QObject::connect(store, &WorkflowStore::stateChanged, topBar,
                             showSyncState);
            showSyncState();
        } else {
            topBar->setWorkflowName(QStringLiteral("Demo board"));
            topBar->setSyncState(theme.statusWarning(),
                                 QStringLiteral("Autosave off"));
        }

        auto *shareMenu = new QMenu(topBar);
        QObject::connect(
            shareMenu->addAction(QStringLiteral("Copy workflow JSON")),
            &QAction::triggered, view, [layer, store] {
                const QJsonDocument document(core::workflowToJson(
                    layer->graph(),
                    store ? store->name() : QStringLiteral("Demo board")));
                QGuiApplication::clipboard()->setText(
                    QString::fromUtf8(document.toJson()));
            });
        if (store) {
            QObject::connect(
                shareMenu->addAction(QStringLiteral("Reveal workflow file")),
                &QAction::triggered, view, [store] {
                    store->saveNow();
                    QDesktopServices::openUrl(
                        QUrl::fromLocalFile(store->directory()));
                });
        }
        shareMenu->addSeparator();
        QObject::connect(
            shareMenu->addAction(QStringLiteral("Export Timeline…")),
            &QAction::triggered, exporter, [exporter, view] {
                exporter->exportTimelineInteractive(view);
            });
        QObject::connect(
            shareMenu->addAction(QStringLiteral("Send to DaVinci Resolve")),
            &QAction::triggered, exporter,
            [exporter] { exporter->sendToResolve(); });
        topBar->shareButton->setMenu(shareMenu);

        QObject::connect(topBar->themeButton, &QToolButton::clicked, &themes,
                         [&themes] { themes.cycle(); });

        QObject::connect(
            topBar->settingsButton, &QToolButton::clicked, &window,
            [&window, &themes, chrome, coordinator, store] {
                QDialog dialog(&window);
                dialog.setWindowTitle(QStringLiteral("Settings"));
                auto *column = new QVBoxLayout(&dialog);

                auto *appearance =
                    new QLabel(QStringLiteral("Appearance"), &dialog);
                appearance->setStyleSheet(
                    QStringLiteral("font-weight: 700;"));
                column->addWidget(appearance);
                auto *dark = new QRadioButton(QStringLiteral("Dark"), &dialog);
                auto *light =
                    new QRadioButton(QStringLiteral("Light"), &dialog);
                auto *dim =
                    new QRadioButton(QStringLiteral("Dark Dim"), &dialog);
                switch (themes.theme()) {
                case cutpilot::theme::Theme::Dark:
                    dark->setChecked(true);
                    break;
                case cutpilot::theme::Theme::Light:
                    light->setChecked(true);
                    break;
                case cutpilot::theme::Theme::DarkDim:
                    dim->setChecked(true);
                    break;
                }
                QObject::connect(dark, &QRadioButton::clicked, &themes, [&themes] {
                    themes.setTheme(cutpilot::theme::Theme::Dark);
                });
                QObject::connect(light, &QRadioButton::clicked, &themes, [&themes] {
                    themes.setTheme(cutpilot::theme::Theme::Light);
                });
                QObject::connect(dim, &QRadioButton::clicked, &themes, [&themes] {
                    themes.setTheme(cutpilot::theme::Theme::DarkDim);
                });
                column->addWidget(dark);
                column->addWidget(light);
                column->addWidget(dim);

                auto *keys = new QLabel(QStringLiteral("API keys (BYOK)"),
                                        &dialog);
                keys->setStyleSheet(QStringLiteral("font-weight: 700;"));
                column->addSpacing(8);
                column->addWidget(keys);
                QStringList providers;
                for (const auto &model : coordinator->models()) {
                    if (!model.needsKey || providers.contains(model.provider))
                        continue;
                    providers.push_back(model.provider);
                    auto *row = new QHBoxLayout;
                    row->addWidget(new QLabel(
                        QStringLiteral("%1 — %2").arg(
                            model.provider,
                            model.hasKey
                                ? QStringLiteral("key configured")
                                : QStringLiteral("no key")),
                        &dialog));
                    auto *add = new QPushButton(
                        model.hasKey ? QStringLiteral("Replace key…")
                                     : QStringLiteral("Add key…"),
                        &dialog);
                    QObject::connect(add, &QPushButton::clicked, chrome,
                                     [chrome, provider = model.provider] {
                                         chrome->promptAddKey(-1, provider);
                                     });
                    row->addWidget(add);
                    row->addStretch(1);
                    column->addLayout(row);
                }
                if (providers.isEmpty())
                    column->addWidget(new QLabel(
                        QStringLiteral(
                            "No keyed vendors registered yet — the model "
                            "registry loads when the service is up."),
                        &dialog));

                if (store) {
                    column->addSpacing(8);
                    auto *workspace = new QLabel(
                        QStringLiteral("Workspace: %1").arg(store->directory()),
                        &dialog);
                    workspace->setWordWrap(true);
                    column->addWidget(workspace);
                }

                auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close,
                                                     &dialog);
                QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                                 &QDialog::reject);
                QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                                 &QDialog::accept);
                column->addWidget(buttons);
                dialog.exec();
            });

        // Account: per-provider key state from the live registry, and the
        // add-key flow — BYOK usage state, not a hosted balance.
        auto *accountMenu = new QMenu(topBar);
        QObject::connect(accountMenu, &QMenu::aboutToShow, accountMenu,
                         [accountMenu, coordinator, chrome] {
                             accountMenu->clear();
                             QStringList providers;
                             for (const auto &model : coordinator->models()) {
                                 if (!model.needsKey
                                     || providers.contains(model.provider))
                                     continue;
                                 providers.push_back(model.provider);
                                 QAction *state = accountMenu->addAction(
                                     QStringLiteral("%1 — %2").arg(
                                         model.provider,
                                         model.hasKey
                                             ? QStringLiteral("key configured")
                                             : QStringLiteral("no key")));
                                 state->setEnabled(false);
                                 QObject::connect(
                                     accountMenu->addAction(
                                         QStringLiteral("    Add %1 key…")
                                             .arg(model.provider)),
                                     &QAction::triggered, chrome,
                                     [chrome, provider = model.provider] {
                                         chrome->promptAddKey(-1, provider);
                                     });
                             }
                             if (providers.isEmpty()) {
                                 QAction *empty = accountMenu->addAction(
                                     QStringLiteral(
                                         "Model registry not loaded yet"));
                                 empty->setEnabled(false);
                             }
                         });
        topBar->accountButton->setMenu(accountMenu);

        // ------- One theme repaints the widget chrome and the GPU canvas ---

        QObject::connect(
            &themes, &ThemeController::themeChanged, &window,
            [&themes, view, topBar, rail, pill, cluster, palette, contentPanel,
             searchPanel, assetsPanel, builderPanel, previewPanel, chrome,
             modePages, inspector, transport, quick] {
                const ThemeTable &table = themes.table();
                topBar->retheme(table);
                rail->retheme(table);
                pill->retheme(table);
                cluster->retheme(table);
                quick->retheme(table);
                palette->retheme(table);
                contentPanel->retheme(table);
                searchPanel->retheme(table);
                assetsPanel->retheme(table);
                builderPanel->retheme(table);
                if (previewPanel)
                    previewPanel->retheme(table);
                if (chrome)
                    chrome->retheme(table);
                inspector->retheme(table);
                transport->retheme(table);
                for (ModePage *page : modePages)
                    page->retheme(table);
                view->applyCanvasTheme(themes.theme());
                view->layoutOverlays();
            });
        // Re-tint the sync dot into the new theme.
        QObject::connect(&themes, &ThemeController::themeChanged, topBar,
                         [showSyncState] {
                             if (showSyncState)
                                 showSyncState();
                         });

        // On the seeded compositing board, open the preview ready to compare:
        // the final blend in A, the raw backdrop in B, wipe engaged.
        if (qEnvironmentVariableIntValue("CUTPILOT_COMPOSITE_BOARD") > 0) {
            int blendId = -1;
            int backdropId = -1;
            for (const core::Node &node : layer->graph().nodes()) {
                if (node.kind == core::NodeKind::Blend)
                    blendId = node.id;
                else if (node.kind == core::NodeKind::Still && backdropId == -1)
                    backdropId = node.id;
            }
            if (blendId != -1)
                previews->pin(PreviewController::Buffer::A, blendId);
            if (backdropId != -1)
                previews->pin(PreviewController::Buffer::B, backdropId);
            previewPanel->selectMode(PreviewItem::CompareMode::Wipe);
        }

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

    // CUTPILOT_AUTORUN=<n> runs the whole board as a pipeline n times, back
    // to back, as soon as the model registry lands — a second pass shows the
    // cache serving reused results. With CUTPILOT_SCREENSHOT the capture
    // waits for the last run to settle instead of firing on a fixed delay.
    const int autorunPasses =
        coordinator ? qMax(0, qEnvironmentVariableIntValue("CUTPILOT_AUTORUN")) : 0;
    const QString shotPath = qEnvironmentVariable("CUTPILOT_SCREENSHOT");

    // CUTPILOT_EXPORT_DIR=<dir> exports the board there once the autorun
    // passes settle; with CUTPILOT_SCREENSHOT the capture waits for the
    // export to finish so the status strip shows the export summary.
    const QString exportDir =
        exporter ? qEnvironmentVariable("CUTPILOT_EXPORT_DIR") : QString();

    auto passesLeft = std::make_shared<int>(autorunPasses);
    if (autorunPasses > 0) {
        QObject::connect(
            coordinator, &GenerationCoordinator::modelsReady, view,
            [coordinator] { coordinator->runGraph(); },
            static_cast<Qt::ConnectionType>(Qt::AutoConnection
                                            | Qt::SingleShotConnection));
        QObject::connect(
            coordinator, &GenerationCoordinator::runSummaryChanged, view,
            [coordinator, passesLeft](const cutpilot::ipc::RunSummary &summary) {
                if (summary.active || summary.total == 0 || *passesLeft <= 0)
                    return;
                if (--*passesLeft > 0) {
                    QTimer::singleShot(400, coordinator,
                                       [coordinator] { coordinator->runGraph(); });
                }
            });
    }

    if (!exportDir.isEmpty() && autorunPasses > 0) {
        auto exportStarted = std::make_shared<bool>(false);
        QObject::connect(
            coordinator, &GenerationCoordinator::runSummaryChanged, exporter,
            [exporter, exportDir, exportStarted,
             passesLeft](const cutpilot::ipc::RunSummary &summary) {
                if (*exportStarted || summary.active || summary.total == 0
                    || *passesLeft > 0)
                    return;
                *exportStarted = true;
                QTimer::singleShot(400, exporter, [exporter, exportDir] {
                    exporter->exportTimelineTo(exportDir,
                                               QStringLiteral("cut"));
                });
            });
        QObject::connect(
            exporter, &ExportController::exportFinished, &window,
            [&window, shotPath, statsActive](bool, const QString &) {
                if (shotPath.isEmpty())
                    return;
                QTimer::singleShot(700, &window, [&window, shotPath, statsActive] {
                    window.grab().save(shotPath);
                    if (!statsActive)
                        QCoreApplication::quit();
                });
            });
    } else if (!shotPath.isEmpty() && autorunPasses > 0) {
        auto captured = std::make_shared<bool>(false);
        QObject::connect(
            coordinator, &GenerationCoordinator::runSummaryChanged, &window,
            [&window, shotPath, statsActive, captured,
             passesLeft](const cutpilot::ipc::RunSummary &summary) {
                if (*captured || summary.active || summary.total == 0
                    || *passesLeft > 0)
                    return;
                *captured = true;
                QTimer::singleShot(700, &window, [&window, shotPath, statsActive] {
                    window.grab().save(shotPath);
                    if (!statsActive)
                        QCoreApplication::quit();
                });
            });
    } else if (!shotPath.isEmpty() && coordinator
               && qEnvironmentVariableIntValue("CUTPILOT_QUICK_DEMO") > 0) {
        // The scripted quick pass is a real run; the capture waits for it to
        // settle so the surface shows the finished result.
        auto captured = std::make_shared<bool>(false);
        QObject::connect(
            coordinator, &GenerationCoordinator::runSummaryChanged, &window,
            [&window, shotPath, statsActive,
             captured](const cutpilot::ipc::RunSummary &summary) {
                if (*captured || summary.active || summary.total == 0)
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
