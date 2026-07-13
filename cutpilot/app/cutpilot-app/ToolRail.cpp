#include "ToolRail.h"

#include "cutpilot/theme/ThemeTable.h"

#include <QToolButton>
#include <QVBoxLayout>

namespace cutpilot::app {

namespace {

constexpr int kRailWidth = 44;

QToolButton *makeRailButton(const QString &glyph, const QString &tip,
                            bool checkable, QWidget *parent)
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

ToolRail::ToolRail(const theme::ThemeTable &theme, QWidget *parent)
    : QWidget(parent)
{
    setFixedWidth(kRailWidth);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(5, 8, 5, 8);
    column->setSpacing(6);

    m_content = makeRailButton(
        QStringLiteral("▤"),
        QStringLiteral("Content — the workflow's nodes and results"), true,
        this);
    auto *nodes = makeRailButton(
        QStringLiteral("✚"), QStringLiteral("Nodes — add from the taxonomy"),
        false, this);
    m_search = makeRailButton(
        QStringLiteral("⌕"),
        QStringLiteral("Search — find nodes, models, and assets"), true, this);
    m_assets = makeRailButton(
        QStringLiteral("▦"),
        QStringLiteral("Assets — references to drag onto the canvas"), true,
        this);
    m_builder = makeRailButton(
        QStringLiteral("⧉"),
        QStringLiteral("Builder — reusable templates and saved groups"), true,
        this);

    column->addWidget(m_content);
    column->addWidget(nodes);
    column->addWidget(m_search);
    column->addWidget(m_assets);
    column->addWidget(m_builder);
    column->addStretch(1);

    auto *help = makeRailButton(QStringLiteral("?"),
                                QStringLiteral("Shortcuts and help"), false,
                                this);
    column->addWidget(help);

    connect(nodes, &QToolButton::clicked, this, &ToolRail::nodesRequested);
    connect(help, &QToolButton::clicked, this, &ToolRail::helpRequested);
    connect(m_content, &QToolButton::toggled, this,
            [this](bool checked) { onToggled(Item::Content, checked); });
    connect(m_search, &QToolButton::toggled, this,
            [this](bool checked) { onToggled(Item::Search, checked); });
    connect(m_assets, &QToolButton::toggled, this,
            [this](bool checked) { onToggled(Item::Assets, checked); });
    connect(m_builder, &QToolButton::toggled, this,
            [this](bool checked) { onToggled(Item::Builder, checked); });

    retheme(theme);
}

void ToolRail::retheme(const theme::ThemeTable &theme)
{
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--ToolRail { background-color: %1; "
            "border-right: 1px solid %2; }"
            "QToolButton {"
            "  color: %3; background: transparent;"
            "  border: none; border-radius: 6px; font-size: 15px;"
            "}"
            "QToolButton:hover { background-color: %4; color: %5; }"
            "QToolButton:pressed { background-color: %6; }"
            "QToolButton:focus { border: 2px solid %7; }"
            "QToolButton:checked { background-color: %8; color: %9; }")
            .arg(theme.surface1().name(), theme.borderSubtle().name(),
                 theme.textSecondary().name(), theme.surfaceHover().name(),
                 theme.textPrimary().name(), theme.surfaceActive().name(),
                 theme.borderFocus().name(), theme.emphasis().name(),
                 theme.textOnEmphasis().name()));
}

QToolButton *ToolRail::button(Item item) const
{
    switch (item) {
    case Item::Content:
        return m_content;
    case Item::Search:
        return m_search;
    case Item::Assets:
        return m_assets;
    case Item::Builder:
        return m_builder;
    }
    return nullptr;
}

void ToolRail::onToggled(Item item, bool checked)
{
    if (checked) {
        for (Item other : { Item::Content, Item::Search, Item::Assets,
                            Item::Builder }) {
            if (other != item && button(other)->isChecked())
                button(other)->setChecked(false);
        }
    }
    emit panelToggled(item, checked);
}

void ToolRail::closeAll()
{
    for (Item item :
         { Item::Content, Item::Search, Item::Assets, Item::Builder }) {
        if (button(item)->isChecked())
            button(item)->setChecked(false);
    }
}

} // namespace cutpilot::app
