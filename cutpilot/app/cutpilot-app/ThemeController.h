#pragma once

#include "cutpilot/theme/ThemeTable.h"

#include <QObject>

namespace cutpilot::app {

// The one owner of the resolved theme. Chrome widgets restyle from the table
// on themeChanged, and the same signal pushes the theme into every canvas
// item, so a toggle repaints widget chrome and GPU surfaces identically. The
// chosen theme persists across launches.
class ThemeController : public QObject {
    Q_OBJECT

public:
    explicit ThemeController(QObject *parent = nullptr);

    const theme::ThemeTable &table() const { return m_table; }
    theme::Theme theme() const { return m_table.theme(); }

    void setTheme(theme::Theme themeId);

    // Dark → Light → Dark-Dim → Dark.
    void cycle();

    QString themeName() const;

signals:
    void themeChanged();

private:
    theme::ThemeTable m_table;
};

} // namespace cutpilot::app
