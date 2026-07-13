#include "CommandPalette.h"

#include "cutpilot/theme/ThemeTable.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace cutpilot::app {

namespace {

constexpr int kWidth = 440;
constexpr int kMaxHeight = 420;
constexpr int kRowRole = Qt::UserRole + 1;

} // namespace

CommandPalette::CommandPalette(const theme::ThemeTable &theme, QWidget *parent)
    : QWidget(parent)
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(8, 8, 8, 8);
    column->setSpacing(6);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Search nodes or models"));
    m_search->installEventFilter(this);
    column->addWidget(m_search);

    m_list = new QListWidget(this);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    column->addWidget(m_list, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this] { refill(); });
    connect(m_list, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { activate(item); });

    if (parent)
        parent->installEventFilter(this);

    retheme(theme);
    resize(kWidth, kMaxHeight);
    hide();
}

void CommandPalette::retheme(const theme::ThemeTable &theme)
{
    setStyleSheet(
        QStringLiteral(
            "QWidget { background-color: %1; color: %2; }"
            "QLineEdit {"
            "  background-color: %3; color: %2;"
            "  border: 1px solid %4; border-radius: 4px; padding: 6px 8px;"
            "}"
            "QLineEdit:focus { border: 2px solid %5; }"
            "QListWidget {"
            "  background-color: %1; color: %2;"
            "  border: none; outline: none;"
            "}"
            "QListWidget::item { padding: 5px 8px; border-radius: 4px; }"
            "QListWidget::item:selected { background-color: %6; color: %2; }"
            "QListWidget::item:disabled { color: %7; }")
            .arg(theme.surfaceOverlay().name(), theme.textPrimary().name(),
                 theme.surface3().name(), theme.borderDefault().name(),
                 theme.borderFocus().name(), theme.surfaceActive().name(),
                 theme.textTertiary().name()));
}

void CommandPalette::open()
{
    m_model.clearOffers();
    m_search->clear();
    refill();
    reanchor();
    show();
    raise();
    m_search->setFocus();
}

void CommandPalette::openOffers(const QStringList &titles)
{
    m_model.setOfferTitles(titles);
    m_search->clear();
    refill();
    reanchor();
    show();
    raise();
    m_search->setFocus();
}

void CommandPalette::dismiss()
{
    if (!isVisible())
        return;
    hide();
    emit dismissed();
}

void CommandPalette::refill()
{
    m_rows = m_model.rows(m_search->text());
    m_list->clear();

    QFont sectionFont = font();
    sectionFont.setPointSizeF(sectionFont.pointSizeF() - 1.5);
    sectionFont.setBold(true);
    sectionFont.setCapitalization(QFont::AllUppercase);

    int firstSelectable = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const PaletteModel::Row &row = m_rows[i];
        auto *item = new QListWidgetItem(m_list);
        item->setData(kRowRole, i);
        if (row.kind == PaletteModel::RowKind::Section) {
            item->setText(row.text);
            item->setFont(sectionFont);
            item->setFlags(Qt::NoItemFlags);
        } else {
            item->setText(row.detail.isEmpty()
                              ? row.text
                              : QStringLiteral("%1   ·   %2")
                                    .arg(row.text, row.detail));
            if (firstSelectable == -1)
                firstSelectable = m_list->count() - 1;
        }
    }

    if (m_rows.isEmpty()) {
        auto *item = new QListWidgetItem(
            QStringLiteral("No matches — try a shorter search"), m_list);
        item->setFlags(Qt::NoItemFlags);
    }

    if (firstSelectable != -1)
        m_list->setCurrentRow(firstSelectable);
}

void CommandPalette::moveSelection(int step)
{
    const int count = m_list->count();
    int row = m_list->currentRow();
    for (int i = 0; i < count; ++i) {
        row += step;
        if (row < 0 || row >= count)
            return;
        QListWidgetItem *item = m_list->item(row);
        if (item->flags().testFlag(Qt::ItemIsSelectable)) {
            m_list->setCurrentRow(row);
            m_list->scrollToItem(item);
            return;
        }
    }
}

void CommandPalette::activate(QListWidgetItem *item)
{
    if (!item || !item->flags().testFlag(Qt::ItemIsEnabled))
        return;
    const int index = item->data(kRowRole).toInt();
    if (index < 0 || index >= m_rows.size())
        return;
    const PaletteModel::Row row = m_rows[index];
    hide();
    if (m_model.offersActive())
        emit offerChosen(row.text);
    else
        emit prototypeChosen(row.prototype);
}

void CommandPalette::reanchor()
{
    if (!parentWidget())
        return;
    move((parentWidget()->width() - width()) / 2,
         qMax(24, parentWidget()->height() / 6));
}

bool CommandPalette::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        reanchor();
        return false;
    }

    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Down:
            moveSelection(1);
            return true;
        case Qt::Key_Up:
            moveSelection(-1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activate(m_list->currentItem());
            return true;
        case Qt::Key_Escape:
            dismiss();
            return true;
        default:
            break;
        }
    }

    if (watched == m_search && event->type() == QEvent::FocusOut
        && isVisible()) {
        dismiss();
        return false;
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace cutpilot::app
