#pragma once

#include "PaletteModel.h"

#include "cutpilot/core/Connection.h"
#include "cutpilot/core/Node.h"

#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTreeWidget;
QT_END_NAMESPACE

namespace cutpilot::theme {
class ThemeTable;
}

namespace cutpilot::render {
class NodeLayerItem;
}

namespace cutpilot::app {

// The floating panel every rail item opens: a titled overlay anchored beside
// the rail, closed by its own button or by toggling the rail item off.
class RailPanel : public QWidget {
    Q_OBJECT

public:
    RailPanel(const QString &title, const theme::ThemeTable &theme,
              QWidget *parent);

    virtual void retheme(const theme::ThemeTable &theme);

signals:
    void closeRequested();

protected:
    // The panel's content area, below the title row.
    QWidget *body() const { return m_body; }

private:
    QWidget *m_body = nullptr;
};

// The workflow's nodes and results, grouped by what they are, searchable,
// with a click jumping the camera to the node.
class ContentPanel : public RailPanel {
    Q_OBJECT

public:
    ContentPanel(const theme::ThemeTable &theme, render::NodeLayerItem *layer,
                 QWidget *parent);

    void refresh();

signals:
    void nodeActivated(int nodeId);

private:
    render::NodeLayerItem *m_layer = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
};

// Global search across the board's nodes, the model registry, and the media
// references in use. Nodes and assets jump to the canvas; a model places a
// generation node already set to it.
class SearchPanel : public RailPanel {
    Q_OBJECT

public:
    using ModelsProvider = std::function<QVector<PaletteModel::ModelEntry>()>;

    SearchPanel(const theme::ThemeTable &theme, render::NodeLayerItem *layer,
                ModelsProvider models, QWidget *parent);

    void refresh();

signals:
    void nodeActivated(int nodeId);
    void prototypeChosen(const cutpilot::core::Node &prototype);

private:
    render::NodeLayerItem *m_layer = nullptr;
    ModelsProvider m_models;
    QLineEdit *m_field = nullptr;
    QListWidget *m_results = nullptr;
};

// The project's media references: files added here (or dragged in) become
// draggable sources; dropping one on the canvas — or double-clicking it —
// places a still or video node carrying that file.
class AssetsPanel : public RailPanel {
    Q_OBJECT

public:
    AssetsPanel(const theme::ThemeTable &theme, QWidget *parent);

    // The ready-to-place node for a media file: a video node for video
    // suffixes, a still image node otherwise, titled by file name.
    static core::Node prototypeForFile(const QString &path);

    static bool isMediaFile(const QString &path);

    void refresh();

signals:
    void assetChosen(const QString &path);

private:
    void addFiles();
    void removeSelected();
    QStringList storedPaths() const;
    void storePaths(const QStringList &paths);

    QListWidget *m_list = nullptr;
};

// Reusable building blocks: built-in starters plus templates saved from the
// current selection, each dropping onto the canvas as one wired unit.
class BuilderPanel : public RailPanel {
    Q_OBJECT

public:
    BuilderPanel(const theme::ThemeTable &theme, render::NodeLayerItem *layer,
                 const QString &templatesDirectory, QWidget *parent);

    void refresh();

signals:
    void templateChosen(const QVector<cutpilot::core::Node> &prototypes,
                        const QVector<cutpilot::core::Connection> &indexWires);

private:
    void saveSelection();
    void activate(QListWidgetItem *item);

    render::NodeLayerItem *m_layer = nullptr;
    QString m_directory;
    QListWidget *m_list = nullptr;
};

} // namespace cutpilot::app
