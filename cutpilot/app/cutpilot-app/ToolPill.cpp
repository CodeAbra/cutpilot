#include "ToolPill.h"

#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>

namespace cutpilot::app {

namespace {

QToolButton *makeTool(const QString &glyph, const QString &tip, bool checkable,
                      QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(glyph);
    button->setToolTip(tip);
    button->setCheckable(checkable);
    button->setFixedSize(34, 34);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

} // namespace

ToolPill::ToolPill(const theme::ThemeTable &theme,
                   render::NodeLayerItem *layer, QWidget *parent)
    : QWidget(parent)
    , m_layer(layer)
{
    setAttribute(Qt::WA_StyledBackground, true);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(4);

    m_cursor = makeTool(QStringLiteral("◉"),
                        QStringLiteral("Cursor — select, move, marquee"), true,
                        this);
    m_cut = makeTool(QStringLiteral("✄"),
                     QStringLiteral("Cut — slice a connector"), true, this);
    m_image = makeTool(QStringLiteral("▣"),
                       QStringLiteral("Image — place a generate-image node"),
                       true, this);
    m_video = makeTool(QStringLiteral("▶"),
                       QStringLiteral("Video — place a video node"), true,
                       this);
    m_voice = makeTool(QStringLiteral("♪"),
                       QStringLiteral("Voice — place an audio node"), true,
                       this);
    m_adjust = makeTool(
        QStringLiteral("◧"),
        QStringLiteral("Adjust — place a compositing node"), true, this);
    m_frame = makeTool(QStringLiteral("⬚"),
                       QStringLiteral("Frame — drop a grouping backdrop"),
                       true, this);
    auto *nodes = makeTool(QStringLiteral("⊞"),
                           QStringLiteral("Nodes — search the taxonomy"),
                           false, this);
    auto *upload = makeTool(QStringLiteral("⬆"),
                            QStringLiteral("Upload — bring in a local file"),
                            false, this);

    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setProperty("hairline", true);

    m_connect = makeTool(QStringLiteral("⌁"),
                         QStringLiteral("Connect — draw a connector"), true,
                         this);
    // The variation selector keeps the bolt a monochrome text glyph — the
    // chrome carries no colored element but the focus ring.
    m_quick = makeTool(QStringLiteral("\u26A1\uFE0E"),
                       QStringLiteral("Quick Mode — one-prompt flow"), true,
                       this);
    m_quick->setProperty("quick", true);

    for (QToolButton *button : { m_cursor, m_cut, m_image, m_video, m_voice,
                                 m_adjust, m_frame, nodes, upload })
        row->addWidget(button);
    row->addSpacing(4);
    row->addWidget(divider);
    row->addSpacing(4);
    row->addWidget(m_connect);
    row->addWidget(m_quick);

    // The adjust tool fans out into the compositing operations.
    auto *adjustMenu = new QMenu(this);
    for (const QString &title :
         { QStringLiteral("Blend"), QStringLiteral("Mask"),
           QStringLiteral("Key"), QStringLiteral("Transform") }) {
        adjustMenu->addAction(title, this,
                              [this, title] { arm(title, m_adjust); });
    }
    m_adjust->setMenu(adjustMenu);
    m_adjust->setPopupMode(QToolButton::InstantPopup);

    connect(m_cursor, &QToolButton::clicked, this, [this] {
        m_layer->setTool(render::NodeLayerItem::Tool::Cursor);
        syncFromLayer();
    });
    connect(m_cut, &QToolButton::clicked, this, [this] {
        m_layer->setTool(render::NodeLayerItem::Tool::Cut);
        syncFromLayer();
    });
    connect(m_connect, &QToolButton::clicked, this, [this] {
        m_layer->setTool(render::NodeLayerItem::Tool::Connect);
        syncFromLayer();
    });
    connect(m_image, &QToolButton::clicked, this,
            [this] { arm(QStringLiteral("Generate Image"), m_image); });
    connect(m_video, &QToolButton::clicked, this,
            [this] { arm(QStringLiteral("Video"), m_video); });
    connect(m_voice, &QToolButton::clicked, this,
            [this] { arm(QStringLiteral("Generate Voice"), m_voice); });
    connect(m_frame, &QToolButton::clicked, this,
            [this] { arm(QStringLiteral("Frame"), m_frame); });
    connect(nodes, &QToolButton::clicked, this, &ToolPill::paletteRequested);
    connect(upload, &QToolButton::clicked, this, &ToolPill::uploadRequested);
    connect(m_quick, &QToolButton::toggled, this,
            [this](bool active) { emit quickModeToggled(active); });

    connect(m_layer, &render::NodeLayerItem::toolChanged, this,
            [this] { syncFromLayer(); });

    retheme(theme);
    syncFromLayer();
}

void ToolPill::retheme(const theme::ThemeTable &theme)
{
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--ToolPill {"
            "  background-color: %1; border: 1px solid %2;"
            "  border-radius: 23px;"
            "}"
            "QFrame[hairline=\"true\"] { color: %2; }"
            "QToolButton {"
            "  color: %3; background: transparent;"
            "  border: none; border-radius: 17px; font-size: 15px;"
            "}"
            "QToolButton:hover { background-color: %4; color: %5; }"
            "QToolButton:pressed { background-color: %6; }"
            "QToolButton:focus { border: 2px solid %7; }"
            "QToolButton:checked { background-color: %8; color: %9; }"
            "QToolButton::menu-indicator { image: none; }")
            .arg(theme.surfaceOverlay().name(), theme.borderDefault().name(),
                 theme.textSecondary().name(), theme.surfaceHover().name(),
                 theme.textPrimary().name(), theme.surfaceActive().name(),
                 theme.borderFocus().name(), theme.emphasis().name(),
                 theme.textOnEmphasis().name()));
    adjustSize();
}

bool ToolPill::quickModeActive() const
{
    return m_quick->isChecked();
}

void ToolPill::setQuickModeActive(bool active)
{
    m_quick->setChecked(active);
}

void ToolPill::arm(const QString &catalogTitle, QToolButton *button)
{
    m_armed = button;
    m_layer->armPlacement(core::catalogPrototype(catalogTitle));
    syncFromLayer();
}

void ToolPill::syncFromLayer()
{
    const auto tool = m_layer->tool();
    if (tool != render::NodeLayerItem::Tool::Place)
        m_armed = nullptr;

    m_cursor->setChecked(tool == render::NodeLayerItem::Tool::Cursor);
    m_cut->setChecked(tool == render::NodeLayerItem::Tool::Cut);
    m_connect->setChecked(tool == render::NodeLayerItem::Tool::Connect);
    for (QToolButton *button :
         { m_image, m_video, m_voice, m_adjust, m_frame })
        button->setChecked(tool == render::NodeLayerItem::Tool::Place
                           && button == m_armed);
}

} // namespace cutpilot::app
