#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QLabel;
class QLineEdit;
class QToolButton;
QT_END_NAMESPACE

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::app {

// The top chrome: the six-mode switcher, the inline-editable workflow name,
// the live autosave readout, and the global controls (share, settings, theme,
// account). Pure presentation — the window wires each control to its owner.
class TopBar : public QWidget {
    Q_OBJECT

public:
    static const QStringList &modeNames();

    explicit TopBar(const theme::ThemeTable &theme, QWidget *parent = nullptr);

    void retheme(const theme::ThemeTable &theme);

    int activeMode() const;
    void setActiveMode(int index);

    QString workflowName() const;
    void setWorkflowName(const QString &name);

    // The autosave readout: a colored state dot and a short message.
    void setSyncState(const QColor &dot, const QString &message);

    QToolButton *shareButton = nullptr;
    QToolButton *settingsButton = nullptr;
    QToolButton *themeButton = nullptr;
    QToolButton *accountButton = nullptr;

signals:
    void modeSelected(int index);
    void workflowNameCommitted(const QString &name);

private:
    QButtonGroup *m_modes = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_syncDot = nullptr;
    QLabel *m_syncText = nullptr;
    QColor m_dotColor;
};

} // namespace cutpilot::app
