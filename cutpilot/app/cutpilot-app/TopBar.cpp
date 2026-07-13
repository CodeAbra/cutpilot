#include "TopBar.h"

#include "cutpilot/theme/ThemeTable.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace cutpilot::app {

namespace {

constexpr int kBarHeight = 44;

QToolButton *makeGlobalButton(const QString &glyph, const QString &tip,
                              QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(glyph);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setFixedSize(32, 32);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

} // namespace

const QStringList &TopBar::modeNames()
{
    static const QStringList names = {
        QStringLiteral("Director"),   QStringLiteral("Cinema"),
        QStringLiteral("Storyboard"), QStringLiteral("Timeline"),
        QStringLiteral("Node"),       QStringLiteral("Review"),
    };
    return names;
}

TopBar::TopBar(const theme::ThemeTable &theme, QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(kBarHeight);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(8, 6, 8, 6);
    row->setSpacing(6);

    m_modes = new QButtonGroup(this);
    m_modes->setExclusive(true);
    for (int i = 0; i < modeNames().size(); ++i) {
        auto *tab = new QToolButton(this);
        tab->setText(modeNames()[i]);
        tab->setCheckable(true);
        tab->setMinimumWidth(96);
        tab->setFixedHeight(32);
        tab->setFocusPolicy(Qt::TabFocus);
        tab->setProperty("modeTab", true);
        m_modes->addButton(tab, i);
        row->addWidget(tab);
    }
    connect(m_modes, &QButtonGroup::idClicked, this,
            [this](int id) { emit modeSelected(id); });

    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setProperty("hairline", true);
    row->addSpacing(6);
    row->addWidget(divider);
    row->addSpacing(6);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("Untitled Workflow"));
    m_name->setToolTip(QStringLiteral("Workflow name — click to rename"));
    m_name->setFixedHeight(28);
    m_name->setMaximumWidth(280);
    m_name->setFrame(false);
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        m_name->clearFocus();
        emit workflowNameCommitted(m_name->text());
    });
    row->addWidget(m_name);

    m_syncDot = new QLabel(QStringLiteral("●"), this);
    m_syncText = new QLabel(this);
    row->addSpacing(2);
    row->addWidget(m_syncDot);
    row->addWidget(m_syncText);

    row->addStretch(1);

    shareButton = makeGlobalButton(QStringLiteral("⇪"),
                                   QStringLiteral("Share this workflow"), this);
    shareButton->setPopupMode(QToolButton::InstantPopup);
    settingsButton =
        makeGlobalButton(QStringLiteral("⚙"), QStringLiteral("Settings"), this);
    themeButton = makeGlobalButton(
        QStringLiteral("◐"),
        QStringLiteral("Theme — cycles Dark, Light, Dark-Dim"), this);
    accountButton = makeGlobalButton(
        QStringLiteral("◔"), QStringLiteral("Account — API keys and usage"),
        this);
    accountButton->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(shareButton);
    row->addWidget(settingsButton);
    row->addWidget(themeButton);
    row->addWidget(accountButton);

    retheme(theme);
}

void TopBar::retheme(const theme::ThemeTable &theme)
{
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--TopBar { background-color: %1; }"
            "QLabel { color: %2; background: transparent; }"
            "QFrame[hairline=\"true\"] { color: %3; }"
            "QLineEdit {"
            "  color: %4; background: transparent;"
            "  border: 1px solid transparent; border-radius: 4px;"
            "  padding: 2px 6px; font-weight: 600;"
            "}"
            "QLineEdit:hover { border-color: %5; }"
            "QLineEdit:focus { border: 2px solid %6; background-color: %7; }"
            "QToolButton {"
            "  color: %2; background: transparent;"
            "  border: none; border-radius: 4px; padding: 2px 10px;"
            "}"
            "QToolButton:hover { background-color: %8; color: %4; }"
            "QToolButton:pressed { background-color: %9; }"
            "QToolButton:focus { border: 2px solid %6; }"
            "QToolButton[modeTab=\"true\"]:checked {"
            "  background-color: %10; color: %11; font-weight: 600;"
            "}"
            "QToolButton::menu-indicator { image: none; }")
            .arg(theme.surface2().name(), theme.textSecondary().name(),
                 theme.borderSubtle().name(), theme.textPrimary().name(),
                 theme.borderStrong().name(), theme.borderFocus().name(),
                 theme.surface3().name(), theme.surfaceHover().name(),
                 theme.surfaceActive().name())
            .arg(theme.emphasis().name(), theme.textOnEmphasis().name()));

    if (m_dotColor.isValid())
        m_syncDot->setStyleSheet(
            QStringLiteral("color: %1;").arg(m_dotColor.name()));
}

int TopBar::activeMode() const
{
    return m_modes->checkedId();
}

void TopBar::setActiveMode(int index)
{
    if (QAbstractButton *tab = m_modes->button(index))
        tab->setChecked(true);
}

QString TopBar::workflowName() const
{
    return m_name->text();
}

void TopBar::setWorkflowName(const QString &name)
{
    if (m_name->text() != name)
        m_name->setText(name);
}

void TopBar::setSyncState(const QColor &dot, const QString &message)
{
    m_dotColor = dot;
    m_syncDot->setStyleSheet(QStringLiteral("color: %1;").arg(dot.name()));
    m_syncText->setText(message);
}

} // namespace cutpilot::app
