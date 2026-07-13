#pragma once

#include "PaletteModel.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QListWidget;
class QListWidgetItem;
QT_END_NAMESPACE

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::app {

// The "Search nodes or models" popover: a centered, keyboard-first list over
// the canvas fed by PaletteModel. In taxonomy mode a chosen row emits its
// ready-to-place prototype; in offer mode (a connector dropped on empty
// canvas) it emits the chosen compatible title so the canvas can place and
// wire it as one step.
class CommandPalette : public QWidget {
    Q_OBJECT

public:
    explicit CommandPalette(const theme::ThemeTable &theme, QWidget *parent);

    PaletteModel &model() { return m_model; }

    void retheme(const theme::ThemeTable &theme);

    // Open in taxonomy mode; the caller remembers where the pick should land.
    void open();

    // Open in offer mode over the given compatible titles.
    void openOffers(const QStringList &titles);

    void dismiss();

signals:
    void prototypeChosen(const cutpilot::core::Node &prototype);
    void offerChosen(const QString &title);
    void dismissed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refill();
    void moveSelection(int step);
    void activate(QListWidgetItem *item);
    void reanchor();

    PaletteModel m_model;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QVector<PaletteModel::Row> m_rows;
};

} // namespace cutpilot::app
