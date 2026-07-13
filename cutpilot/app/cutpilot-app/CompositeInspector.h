#pragma once

#include "cutpilot/core/Node.h"

#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
class QLabel;
class QSlider;
QT_END_NAMESPACE

namespace cutpilot::render {
class NodeLayerItem;
class PreviewController;
}

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::app {

// The parameter inspector for a compositing node, floated over the canvas's
// right edge. Sliders write parameters live — the preview follows in real
// time — and each finished gesture lands as one undoable step; discrete
// controls (mode, invert, color) commit immediately.
class CompositeInspector : public QWidget {
public:
    CompositeInspector(const theme::ThemeTable &theme,
                       render::NodeLayerItem *layer,
                       render::PreviewController *previews, QWidget *parent);

    void openFor(int nodeId);

    void retheme(const theme::ThemeTable &theme);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void reanchor();

    // Live feedback: write the values and let the preview re-render its
    // affected passes; nothing lands on the undo stack yet.
    void preview();

    // One finished gesture becomes one undo step.
    void commit();

    QSlider *addSlider(const QString &label, int min, int max, int value,
                       const std::function<void(int)> &apply);
    void rebuildControls(core::NodeKind kind);

    render::NodeLayerItem *m_layer = nullptr;
    render::PreviewController *m_previews = nullptr;
    QLabel *m_title = nullptr;
    QWidget *m_controls = nullptr;
    int m_nodeId = -1;
    // The bound node's contentRevision as of this panel's last read or
    // write; a mismatch on graphMutated means someone else moved the values.
    int m_seenRevision = -1;
    core::CompositeParams m_before;
    core::CompositeParams m_current;
};

} // namespace cutpilot::app
