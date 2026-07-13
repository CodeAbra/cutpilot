#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QToolButton;
QT_END_NAMESPACE

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::render {
class CanvasController;
class NodeLayerItem;
}

namespace cutpilot::app {

// The bottom-left canvas cluster: the minimap toggle, undo and redo over the
// real history, and the live zoom control whose menu jumps to Fit, presets,
// or a typed value on the shared camera.
class CanvasCluster : public QWidget {
    Q_OBJECT

public:
    CanvasCluster(const theme::ThemeTable &theme,
                  render::NodeLayerItem *layer,
                  render::CanvasController *controller, QWidget *parent);

    void retheme(const theme::ThemeTable &theme);

    bool minimapVisible() const;
    void setMinimapVisible(bool visible);

    // Frame the whole board (or reset an empty one).
    void fitAll();

signals:
    void minimapToggled(bool visible);

private:
    void setZoomPercent(qreal percent);
    void promptZoomValue();
    void syncHistoryButtons();
    void syncZoomLabel();

    // The canvas viewport in physical pixels, and its device pixel ratio.
    QSizeF viewportPx() const;
    qreal dpr() const;

    render::NodeLayerItem *m_layer = nullptr;
    render::CanvasController *m_controller = nullptr;
    QToolButton *m_minimap = nullptr;
    QToolButton *m_undo = nullptr;
    QToolButton *m_redo = nullptr;
    QToolButton *m_zoom = nullptr;
};

} // namespace cutpilot::app
