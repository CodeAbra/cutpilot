#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QToolButton;
QT_END_NAMESPACE

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::app {

// The left icon rail: Content, Nodes, Search, Assets, and Builder, plus a
// help footer. Panel items are exclusive toggles — opening one closes the
// others — while Nodes fires the command palette directly.
class ToolRail : public QWidget {
    Q_OBJECT

public:
    enum class Item {
        Content,
        Search,
        Assets,
        Builder
    };
    Q_ENUM(Item)

    explicit ToolRail(const theme::ThemeTable &theme, QWidget *parent = nullptr);

    void retheme(const theme::ThemeTable &theme);

    // Programmatic close (a panel dismissed itself); keeps buttons in step.
    void closeAll();

signals:
    void panelToggled(cutpilot::app::ToolRail::Item item, bool open);
    void nodesRequested();
    void helpRequested();

private:
    QToolButton *button(Item item) const;
    void onToggled(Item item, bool checked);

    QToolButton *m_content = nullptr;
    QToolButton *m_search = nullptr;
    QToolButton *m_assets = nullptr;
    QToolButton *m_builder = nullptr;
};

} // namespace cutpilot::app
