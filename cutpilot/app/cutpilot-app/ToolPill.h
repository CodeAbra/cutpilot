#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QToolButton;
QT_END_NAMESPACE

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::render {
class NodeLayerItem;
}

namespace cutpilot::app {

// The bottom-center tool pill: cursor, cut, the add tools (image, video,
// voice, adjust, frame), nodes, upload, and the separated connect + quick
// group. Sticky tools drive the canvas's tool state directly; add tools arm
// a one-shot placement; nodes, upload, and quick raise signals for the window
// to serve. The pill mirrors the canvas's tool so the active control always
// reads true, wherever the tool changed.
class ToolPill : public QWidget {
    Q_OBJECT

public:
    ToolPill(const theme::ThemeTable &theme, render::NodeLayerItem *layer,
             QWidget *parent);

    void retheme(const theme::ThemeTable &theme);

    bool quickModeActive() const;
    void setQuickModeActive(bool active);

signals:
    void paletteRequested();
    void uploadRequested();
    void quickModeToggled(bool active);

private:
    void syncFromLayer();
    void arm(const QString &catalogTitle, QToolButton *button);

    render::NodeLayerItem *m_layer = nullptr;
    QToolButton *m_cursor = nullptr;
    QToolButton *m_cut = nullptr;
    QToolButton *m_image = nullptr;
    QToolButton *m_video = nullptr;
    QToolButton *m_voice = nullptr;
    QToolButton *m_adjust = nullptr;
    QToolButton *m_frame = nullptr;
    QToolButton *m_connect = nullptr;
    QToolButton *m_quick = nullptr;
    QToolButton *m_armed = nullptr;
};

} // namespace cutpilot::app
