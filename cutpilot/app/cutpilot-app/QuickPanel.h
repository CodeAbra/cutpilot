#pragma once

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QWidget>

#include "cutpilot/theme/ThemeTable.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QToolButton;
QT_END_NAMESPACE

namespace cutpilot::render {
class NodeLayerItem;
}

namespace cutpilot::ipc {
class GenerationCoordinator;
}

namespace cutpilot::app {

// The output size for a quick generation: the aspect ratio is width:height,
// the tier is the short side in pixels, sides are rounded to even and held
// inside the generation service's accepted range.
QSize quickOutputSize(int shortSide, int aspectWidth, int aspectHeight);

// The one-prompt surface floated over the canvas: a prompt field, inline
// format/aspect/preset chips, a model chip over the live registry, one run
// control, and the run's status, cost, and result — all bound to one real
// generate node in the document. Every edit lands on that node through the
// undoable command path and every run drives the shared coordinator, so
// leaving the quick surface simply reveals the node, its wiring seams, and
// its result on the full canvas.
class QuickPanel : public QWidget {
    Q_OBJECT

public:
    QuickPanel(const theme::ThemeTable &theme, render::NodeLayerItem *layer,
               ipc::GenerationCoordinator *coordinator, QWidget *parent);

    void retheme(const theme::ThemeTable &theme);

    // Present the surface: adopt the live quick node if one exists, otherwise
    // materialize one centered on the world point, as one undo step.
    void openAt(const QPointF &worldCentre);

    // Close the surface quietly, leaving the node and any result in place.
    void dismiss();

    // The bound node's id, or -1 while no quick node exists.
    int nodeId() const { return m_nodeId; }

    // The durable identity the surface adopts by. The document's stored
    // binding is seeded here on load; materializing or re-adopting announces
    // the binding through boundNodeUidChanged so the document can keep it.
    QString boundNodeUid() const { return m_boundUid; }
    void setBoundNodeUid(const QString &uid) { m_boundUid = uid; }

    // Hand the field's text to the node through the undoable prompt command.
    void commitPrompt();

    // Apply a resolution tier / aspect / model choice to the node. The chip
    // menus land here; the current tier or aspect is kept when only the other
    // half changes.
    void applyTier(int shortSide);
    void applyAspect(int aspectWidth, int aspectHeight);
    void applyPreset(int shortSide, int aspectWidth, int aspectHeight);
    void applyModel(const QString &modelId, const QString &modelLabel);

signals:
    // The user left the quick surface (close control or Escape).
    void dismissed();

    // The surface bound itself to a node and the camera should frame it.
    void nodeFocusRequested(int nodeId);

    // The bound node's model wants a vendor key.
    void addKeyRequested(int nodeId, const QString &provider);

    // The surface owns a different node than before; the document records
    // the identity so adoption survives a relaunch.
    void boundNodeUidChanged(const QString &uid);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void leaveByUser();
    void reanchor();
    void rebuildModelMenu();
    void syncFromNode();
    void syncStatus();
    void syncChips();
    void syncResult();
    void pressRun();
    QString providerForModel(const QString &modelId) const;

    render::NodeLayerItem *m_layer = nullptr;
    ipc::GenerationCoordinator *m_coordinator = nullptr;
    theme::ThemeTable m_theme{ theme::Theme::Dark };
    int m_nodeId = -1;
    QString m_boundUid;
    QImage m_resultImage;

    QToolButton *m_close = nullptr;
    QLabel *m_result = nullptr;
    QPlainTextEdit *m_prompt = nullptr;
    QToolButton *m_formatChip = nullptr;
    QToolButton *m_aspectChip = nullptr;
    QToolButton *m_presetsChip = nullptr;
    QToolButton *m_modelChip = nullptr;
    QMenu *m_modelMenu = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_addKey = nullptr;
    QPushButton *m_run = nullptr;
};

} // namespace cutpilot::app
