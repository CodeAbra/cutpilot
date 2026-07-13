#include "CanvasCluster.h"

#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QToolButton>

namespace cutpilot::app {

namespace {

QToolButton *makeButton(const QString &glyph, const QString &tip,
                        QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(glyph);
    button->setToolTip(tip);
    button->setFixedSize(30, 30);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

} // namespace

CanvasCluster::CanvasCluster(const theme::ThemeTable &theme,
                             render::NodeLayerItem *layer,
                             render::CanvasController *controller,
                             QWidget *parent)
    : QWidget(parent)
    , m_layer(layer)
    , m_controller(controller)
{
    setAttribute(Qt::WA_StyledBackground, true);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(8, 5, 8, 5);
    row->setSpacing(4);

    m_minimap = makeButton(QStringLiteral("▧"),
                           QStringLiteral("Show or hide the minimap (M)"),
                           this);
    m_minimap->setCheckable(true);
    m_minimap->setChecked(true);
    m_undo = makeButton(QStringLiteral("↶"), QStringLiteral("Undo"), this);
    m_redo = makeButton(QStringLiteral("↷"), QStringLiteral("Redo"), this);

    m_zoom = new QToolButton(this);
    m_zoom->setToolTip(
        QStringLiteral("Zoom — fit, presets, or a typed value"));
    m_zoom->setFixedHeight(30);
    m_zoom->setPopupMode(QToolButton::InstantPopup);
    m_zoom->setFocusPolicy(Qt::TabFocus);
    QFont mono;
    mono.setFamilies({ QStringLiteral("SF Mono"), QStringLiteral("Menlo"),
                       QStringLiteral("Cascadia Code") });
    mono.setStyleHint(QFont::Monospace);
    mono.setPixelSize(12);
    m_zoom->setFont(mono);

    auto *zoomMenu = new QMenu(this);
    zoomMenu->addAction(QStringLiteral("Fit"), this, [this] { fitAll(); });
    zoomMenu->addAction(QStringLiteral("50%"), this,
                        [this] { setZoomPercent(50.0); });
    zoomMenu->addAction(QStringLiteral("100%"), this,
                        [this] { setZoomPercent(100.0); });
    zoomMenu->addAction(QStringLiteral("200%"), this,
                        [this] { setZoomPercent(200.0); });
    zoomMenu->addSeparator();
    zoomMenu->addAction(QStringLiteral("Zoom to…"), this,
                        [this] { promptZoomValue(); });
    m_zoom->setMenu(zoomMenu);

    row->addWidget(m_minimap);
    row->addWidget(m_undo);
    row->addWidget(m_redo);
    row->addWidget(m_zoom);

    connect(m_minimap, &QToolButton::toggled, this,
            &CanvasCluster::minimapToggled);
    connect(m_undo, &QToolButton::clicked, this, [this] { m_layer->undo(); });
    connect(m_redo, &QToolButton::clicked, this, [this] { m_layer->redo(); });
    connect(m_layer, &render::NodeLayerItem::graphMutated, this,
            [this] { syncHistoryButtons(); });
    connect(m_controller, &render::CanvasController::cameraChanged, this,
            [this] { syncZoomLabel(); });

    retheme(theme);
    syncHistoryButtons();
    syncZoomLabel();
}

void CanvasCluster::retheme(const theme::ThemeTable &theme)
{
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--CanvasCluster {"
            "  background-color: %1; border: 1px solid %2;"
            "  border-radius: 8px;"
            "}"
            "QToolButton {"
            "  color: %3; background: transparent;"
            "  border: none; border-radius: 6px; padding: 0px 8px;"
            "}"
            "QToolButton:hover { background-color: %4; color: %5; }"
            "QToolButton:pressed { background-color: %6; }"
            "QToolButton:focus { border: 2px solid %7; }"
            "QToolButton:checked { background-color: %8; color: %9; }"
            "QToolButton:disabled { color: %10; }"
            "QToolButton::menu-indicator { image: none; }")
            .arg(theme.surfaceOverlay().name(), theme.borderDefault().name(),
                 theme.textSecondary().name(), theme.surfaceHover().name(),
                 theme.textPrimary().name(), theme.surfaceActive().name(),
                 theme.borderFocus().name(), theme.emphasis().name(),
                 theme.textOnEmphasis().name())
            .arg(theme.textDisabled().name()));
    adjustSize();
}

bool CanvasCluster::minimapVisible() const
{
    return m_minimap->isChecked();
}

void CanvasCluster::setMinimapVisible(bool visible)
{
    m_minimap->setChecked(visible);
}

QSizeF CanvasCluster::viewportPx() const
{
    return QSizeF(m_layer->width(), m_layer->height()) * dpr();
}

qreal CanvasCluster::dpr() const
{
    return devicePixelRatioF();
}

void CanvasCluster::fitAll()
{
    const QRectF bounds = m_layer->contentWorldBounds();
    if (bounds.isNull()) {
        m_controller->reset();
        return;
    }
    m_controller->fitWorldRect(bounds, viewportPx(), dpr());
}

void CanvasCluster::setZoomPercent(qreal percent)
{
    const QSizeF viewport = viewportPx();
    m_controller->setZoomPercent(
        percent, QPointF(viewport.width() / 2.0, viewport.height() / 2.0),
        dpr());
}

void CanvasCluster::promptZoomValue()
{
    bool accepted = false;
    const int percent = QInputDialog::getInt(
        this, QStringLiteral("Zoom"), QStringLiteral("Zoom percentage"),
        qRound(m_controller->zoomPercent()), 8, 400, 5, &accepted);
    if (accepted)
        setZoomPercent(percent);
}

void CanvasCluster::syncHistoryButtons()
{
    m_undo->setEnabled(m_layer->canUndo());
    m_redo->setEnabled(m_layer->canRedo());
}

void CanvasCluster::syncZoomLabel()
{
    m_zoom->setText(
        QStringLiteral("%1% ▾").arg(qRound(m_controller->zoomPercent())));
    adjustSize();
}

} // namespace cutpilot::app
