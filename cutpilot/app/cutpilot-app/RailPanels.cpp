#include "RailPanels.h"

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/core/WorkflowJson.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace cutpilot::app {

namespace core = cutpilot::core;

namespace {

constexpr int kPanelWidth = 300;
constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kNodeIdRole = Qt::UserRole + 2;
constexpr int kKindRole = Qt::UserRole + 3;
constexpr int kTemplateRole = Qt::UserRole + 4;

const QLatin1String kAssetsKey("assets/references");

// The group a node lists under in the Content panel.
QString groupFor(core::NodeKind kind)
{
    switch (kind) {
    case core::NodeKind::Prompt:
        return QStringLiteral("Text");
    case core::NodeKind::Generate:
        return QStringLiteral("Generation");
    case core::NodeKind::Still:
    case core::NodeKind::Video:
        return QStringLiteral("Media");
    case core::NodeKind::Blend:
    case core::NodeKind::Mask:
    case core::NodeKind::Key:
    case core::NodeKind::Transform:
        return QStringLiteral("Compositing");
    case core::NodeKind::CostGate:
        return QStringLiteral("Control");
    case core::NodeKind::Frame:
        return QStringLiteral("Layout");
    case core::NodeKind::Blank:
        break;
    }
    return QStringLiteral("Other");
}

QString statusFor(const core::Node &node)
{
    switch (node.runState) {
    case core::RunState::Queued:
        return QStringLiteral("queued");
    case core::RunState::Running:
        return QStringLiteral("running");
    case core::RunState::Done:
        return QStringLiteral("done");
    case core::RunState::Error:
        return QStringLiteral("error");
    case core::RunState::NeedsKey:
        return QStringLiteral("needs key");
    case core::RunState::Held:
        return QStringLiteral("held");
    case core::RunState::Idle:
        break;
    }
    return QString();
}

// Assets rows drag as file URLs so the canvas can accept panel drags and
// Finder drops through one code path.
class AssetList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
    {
        auto *mime = new QMimeData;
        QList<QUrl> urls;
        for (const QListWidgetItem *item : items)
            urls.push_back(
                QUrl::fromLocalFile(item->data(kPathRole).toString()));
        mime->setUrls(urls);
        return mime;
    }
};

// A selection captured as a template: the selected nodes normalized to their
// joint origin, and the connections running among them re-expressed against
// prototype indices.
void captureSelection(const core::NodeGraph &graph,
                      QVector<core::Node> &prototypes,
                      QVector<core::Connection> &indexWires)
{
    QHash<int, int> indexById;
    QRectF bounds;
    for (const core::Node &node : graph.nodes()) {
        if (!node.selected)
            continue;
        indexById.insert(node.id, prototypes.size());
        core::Node prototype = node;
        prototype.id = 0;
        prototype.selected = false;
        prototype.runState = core::RunState::Idle;
        prototype.runProgress = 0.0;
        prototype.statusMessage.clear();
        prototypes.push_back(prototype);
        bounds = bounds.isNull() ? node.worldRect()
                                 : bounds.united(node.worldRect());
    }
    for (core::Node &prototype : prototypes)
        prototype.worldPos -= bounds.topLeft();

    for (const core::Connection &connection : graph.connections()) {
        if (!indexById.contains(connection.fromNodeId)
            || !indexById.contains(connection.toNodeId))
            continue;
        core::Connection wire;
        wire.fromNodeId = indexById.value(connection.fromNodeId);
        wire.fromPortIndex = connection.fromPortIndex;
        wire.toNodeId = indexById.value(connection.toNodeId);
        wire.toPortIndex = connection.toPortIndex;
        indexWires.push_back(wire);
    }
}

// A template document reuses the workflow format: nodes and wires land in a
// scratch graph for serialization, and load back the same way.
QJsonObject templateToJson(const QVector<core::Node> &prototypes,
                           const QVector<core::Connection> &indexWires,
                           const QString &name)
{
    core::NodeGraph scratch;
    QVector<int> ids;
    for (const core::Node &prototype : prototypes)
        ids.push_back(scratch.addNode(prototype));
    for (const core::Connection &wire : indexWires) {
        core::Connection connection = wire;
        connection.fromNodeId = ids.value(wire.fromNodeId, -1);
        connection.toNodeId = ids.value(wire.toNodeId, -1);
        scratch.addConnection(connection);
    }
    return core::workflowToJson(scratch, name);
}

bool templateFromJson(const QJsonObject &json, QVector<core::Node> &prototypes,
                      QVector<core::Connection> &indexWires, QString *name)
{
    core::NodeGraph scratch;
    if (!core::workflowFromJson(json, scratch, name))
        return false;
    QHash<int, int> indexById;
    for (const core::Node &node : scratch.nodes()) {
        indexById.insert(node.id, prototypes.size());
        core::Node prototype = node;
        prototype.id = 0;
        prototypes.push_back(prototype);
    }
    for (const core::Connection &connection : scratch.connections()) {
        core::Connection wire;
        wire.fromNodeId = indexById.value(connection.fromNodeId);
        wire.fromPortIndex = connection.fromPortIndex;
        wire.toNodeId = indexById.value(connection.toNodeId);
        wire.toPortIndex = connection.toPortIndex;
        indexWires.push_back(wire);
    }
    return true;
}

struct BuiltinTemplate {
    QString name;
    QVector<core::Node> prototypes;
    QVector<core::Connection> indexWires;
};

QVector<BuiltinTemplate> builtinTemplates()
{
    QVector<BuiltinTemplate> templates;

    {
        BuiltinTemplate starter;
        starter.name = QStringLiteral("Starter pipeline");
        core::Node prompt = core::catalogPrototype(QStringLiteral("Prompt"));
        prompt.promptText =
            QStringLiteral("A lighthouse on a stormy cliff at dusk, cinematic");
        prompt.worldPos = QPointF(0.0, 60.0);
        core::Node generate =
            core::catalogPrototype(QStringLiteral("Generate Image"));
        generate.worldPos = QPointF(400.0, 0.0);
        core::Node upscale =
            core::catalogPrototype(QStringLiteral("Upscale Image"));
        upscale.worldPos = QPointF(800.0, 20.0);
        starter.prototypes = { prompt, generate, upscale };
        core::Connection promptWire;
        promptWire.fromNodeId = 0;
        promptWire.fromPortIndex = 0;
        promptWire.toNodeId = 1;
        promptWire.toPortIndex = 1;
        core::Connection resultWire;
        resultWire.fromNodeId = 1;
        resultWire.fromPortIndex = 3;
        resultWire.toNodeId = 2;
        resultWire.toPortIndex = 0;
        starter.indexWires = { promptWire, resultWire };
        templates.push_back(starter);
    }

    {
        BuiltinTemplate composite;
        composite.name = QStringLiteral("Compositing chain");
        core::Node backdrop = core::compositeNodePrototype(core::NodeKind::Still);
        backdrop.title = QStringLiteral("Backdrop");
        backdrop.worldPos = QPointF(0.0, 0.0);
        core::Node subject = core::compositeNodePrototype(core::NodeKind::Still);
        subject.title = QStringLiteral("Subject");
        subject.worldPos = QPointF(0.0, 300.0);
        core::Node key = core::compositeNodePrototype(core::NodeKind::Key);
        key.worldPos = QPointF(380.0, 300.0);
        core::Node transform =
            core::compositeNodePrototype(core::NodeKind::Transform);
        transform.worldPos = QPointF(760.0, 300.0);
        core::Node blend = core::compositeNodePrototype(core::NodeKind::Blend);
        blend.worldPos = QPointF(1140.0, 140.0);
        composite.prototypes = { backdrop, subject, key, transform, blend };
        const auto wire = [](int from, int fromPort, int to, int toPort) {
            core::Connection connection;
            connection.fromNodeId = from;
            connection.fromPortIndex = fromPort;
            connection.toNodeId = to;
            connection.toPortIndex = toPort;
            return connection;
        };
        composite.indexWires = { wire(1, 0, 2, 0), wire(2, 1, 3, 0),
                                 wire(0, 0, 4, 0), wire(3, 1, 4, 1) };
        templates.push_back(composite);
    }

    return templates;
}

} // namespace

RailPanel::RailPanel(const QString &title, const theme::ThemeTable &theme,
                     QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(kPanelWidth);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(10, 8, 10, 10);
    column->setSpacing(8);

    auto *header = new QHBoxLayout;
    auto *heading = new QLabel(title, this);
    heading->setProperty("panelTitle", true);
    auto *close = new QPushButton(QStringLiteral("✕"), this);
    close->setFixedSize(24, 24);
    close->setFlat(true);
    connect(close, &QPushButton::clicked, this, &RailPanel::closeRequested);
    header->addWidget(heading, 1);
    header->addWidget(close);
    column->addLayout(header);

    m_body = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(8);
    column->addWidget(m_body, 1);

    retheme(theme);
    hide();
}

void RailPanel::retheme(const theme::ThemeTable &theme)
{
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--RailPanel, cutpilot--app--ContentPanel,"
            "cutpilot--app--SearchPanel, cutpilot--app--AssetsPanel,"
            "cutpilot--app--BuilderPanel {"
            "  background-color: %1; border: 1px solid %2; border-radius: 8px;"
            "}"
            "QWidget { color: %3; background: transparent; }"
            "QLabel[panelTitle=\"true\"] {"
            "  color: %3; font-weight: 700; letter-spacing: 0.04em;"
            "}"
            "QLineEdit {"
            "  background-color: %4; color: %3;"
            "  border: 1px solid %5; border-radius: 4px; padding: 4px 8px;"
            "}"
            "QLineEdit:focus { border: 2px solid %6; }"
            "QPushButton {"
            "  color: %3; background-color: %4;"
            "  border: 1px solid %5; border-radius: 4px; padding: 4px 10px;"
            "}"
            "QPushButton:hover { background-color: %7; }"
            "QPushButton:flat { background: transparent; border: none; }"
            "QListWidget, QTreeWidget {"
            "  background-color: %1; border: none; outline: none;"
            "}"
            "QListWidget::item, QTreeWidget::item {"
            "  padding: 4px 6px; border-radius: 4px;"
            "}"
            "QListWidget::item:selected, QTreeWidget::item:selected {"
            "  background-color: %8; color: %3;"
            "}"
            "QListWidget::item:disabled { color: %9; }")
            .arg(theme.surfaceOverlay().name(), theme.borderDefault().name(),
                 theme.textPrimary().name(), theme.surface3().name(),
                 theme.borderDefault().name(), theme.borderFocus().name(),
                 theme.surfaceHover().name(), theme.surfaceActive().name(),
                 theme.textTertiary().name()));
}

ContentPanel::ContentPanel(const theme::ThemeTable &theme,
                           render::NodeLayerItem *layer, QWidget *parent)
    : RailPanel(QStringLiteral("Content"), theme, parent)
    , m_layer(layer)
{
    auto *layout = static_cast<QVBoxLayout *>(body()->layout());

    m_filter = new QLineEdit(body());
    m_filter->setPlaceholderText(QStringLiteral("Filter nodes"));
    layout->addWidget(m_filter);

    m_tree = new QTreeWidget(body());
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setExpandsOnDoubleClick(false);
    layout->addWidget(m_tree, 1);

    connect(m_filter, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(m_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
                const QVariant id = item->data(0, kNodeIdRole);
                if (id.isValid())
                    emit nodeActivated(id.toInt());
            });

    // Structure and status edits arrive in bursts; one deferred refresh
    // per burst keeps the tree cheap.
    auto *refreshTimer = new QTimer(this);
    refreshTimer->setSingleShot(true);
    refreshTimer->setInterval(120);
    connect(refreshTimer, &QTimer::timeout, this, [this] { refresh(); });
    connect(m_layer, &render::NodeLayerItem::graphMutated, refreshTimer,
            [refreshTimer, this] {
                if (isVisible())
                    refreshTimer->start();
            });

    refresh();
}

void ContentPanel::refresh()
{
    const QString needle = m_filter->text().trimmed();
    m_tree->clear();

    QHash<QString, QTreeWidgetItem *> groups;
    for (const core::Node &node : m_layer->graph().nodes()) {
        if (!needle.isEmpty()
            && !node.title.contains(needle, Qt::CaseInsensitive)
            && !node.modelLabel.contains(needle, Qt::CaseInsensitive))
            continue;
        const QString group = groupFor(node.kind);
        QTreeWidgetItem *parent = groups.value(group);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree, { group });
            parent->setFlags(Qt::ItemIsEnabled);
            groups.insert(group, parent);
        }
        QString label = node.title;
        const QString status = statusFor(node);
        if (!status.isEmpty())
            label += QStringLiteral("  ·  %1").arg(status);
        auto *item = new QTreeWidgetItem(parent, { label });
        item->setData(0, kNodeIdRole, node.id);
    }
    m_tree->expandAll();
}

SearchPanel::SearchPanel(const theme::ThemeTable &theme,
                         render::NodeLayerItem *layer, ModelsProvider models,
                         QWidget *parent)
    : RailPanel(QStringLiteral("Search"), theme, parent)
    , m_layer(layer)
    , m_models(std::move(models))
{
    auto *layout = static_cast<QVBoxLayout *>(body()->layout());

    m_field = new QLineEdit(body());
    m_field->setPlaceholderText(
        QStringLiteral("Find nodes, models, and assets"));
    layout->addWidget(m_field);

    m_results = new QListWidget(body());
    layout->addWidget(m_results, 1);

    connect(m_field, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(m_results, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                const QVariant nodeId = item->data(kNodeIdRole);
                if (nodeId.isValid()) {
                    emit nodeActivated(nodeId.toInt());
                    return;
                }
                const QVariant kind = item->data(kKindRole);
                if (kind.toString() == QLatin1String("model")) {
                    core::Node prototype =
                        core::catalogPrototype(QStringLiteral("Generate Image"));
                    prototype.modelId = item->data(kPathRole).toString();
                    prototype.modelLabel = item->text().section(
                        QStringLiteral("   ·   "), 0, 0);
                    emit prototypeChosen(prototype);
                }
            });

    refresh();
}

void SearchPanel::refresh()
{
    const QString needle = m_field->text().trimmed();
    m_results->clear();

    const auto addSection = [this](const QString &title) {
        auto *section = new QListWidgetItem(title, m_results);
        section->setFlags(Qt::NoItemFlags);
    };

    // Nodes on the board.
    bool nodesOpen = false;
    for (const core::Node &node : m_layer->graph().nodes()) {
        const bool hit = needle.isEmpty()
            || node.title.contains(needle, Qt::CaseInsensitive)
            || node.modelLabel.contains(needle, Qt::CaseInsensitive)
            || node.promptText.contains(needle, Qt::CaseInsensitive);
        if (!hit)
            continue;
        if (!nodesOpen) {
            nodesOpen = true;
            addSection(QStringLiteral("Nodes"));
        }
        auto *item = new QListWidgetItem(node.title, m_results);
        item->setData(kNodeIdRole, node.id);
    }

    // The model registry.
    bool modelsOpen = false;
    for (const PaletteModel::ModelEntry &model :
         m_models ? m_models() : QVector<PaletteModel::ModelEntry>()) {
        const bool hit = needle.isEmpty()
            || model.label.contains(needle, Qt::CaseInsensitive)
            || model.provider.contains(needle, Qt::CaseInsensitive)
            || model.id.contains(needle, Qt::CaseInsensitive);
        if (!hit)
            continue;
        if (!modelsOpen) {
            modelsOpen = true;
            addSection(QStringLiteral("Models"));
        }
        auto *item = new QListWidgetItem(
            QStringLiteral("%1   ·   %2").arg(model.label, model.provider),
            m_results);
        item->setData(kKindRole, QStringLiteral("model"));
        item->setData(kPathRole, model.id);
    }

    // Media references in use, each jumping to its node.
    bool assetsOpen = false;
    for (const core::Node &node : m_layer->graph().nodes()) {
        if (node.mediaPath.isEmpty())
            continue;
        const QString name = QFileInfo(node.mediaPath).fileName();
        if (!needle.isEmpty() && !name.contains(needle, Qt::CaseInsensitive))
            continue;
        if (!assetsOpen) {
            assetsOpen = true;
            addSection(QStringLiteral("Assets"));
        }
        auto *item = new QListWidgetItem(
            QStringLiteral("%1   ·   %2").arg(name, node.title), m_results);
        item->setData(kNodeIdRole, node.id);
    }
}

AssetsPanel::AssetsPanel(const theme::ThemeTable &theme, QWidget *parent)
    : RailPanel(QStringLiteral("Assets"), theme, parent)
{
    auto *layout = static_cast<QVBoxLayout *>(body()->layout());

    auto *hint = new QLabel(
        QStringLiteral("Drag a file onto the canvas, or double-click to "
                       "place it at the view center."),
        body());
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_list = new AssetList(body());
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    layout->addWidget(m_list, 1);

    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add assets…"), body());
    auto *remove = new QPushButton(QStringLiteral("Remove"), body());
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, [this] { addFiles(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeSelected(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                emit assetChosen(item->data(kPathRole).toString());
            });

    refresh();
}

bool AssetsPanel::isMediaFile(const QString &path)
{
    static const QStringList suffixes = { "png", "jpg",  "jpeg", "webp",
                                          "bmp", "tif",  "tiff", "mp4",
                                          "mov", "m4v",  "webm" };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

core::Node AssetsPanel::prototypeForFile(const QString &path)
{
    static const QStringList videoSuffixes = { "mp4", "mov", "m4v", "webm" };
    const bool video =
        videoSuffixes.contains(QFileInfo(path).suffix().toLower());
    core::Node node = core::compositeNodePrototype(
        video ? core::NodeKind::Video : core::NodeKind::Still);
    node.title = QFileInfo(path).fileName();
    node.mediaPath = path;
    return node;
}

QStringList AssetsPanel::storedPaths() const
{
    return QSettings().value(kAssetsKey).toStringList();
}

void AssetsPanel::storePaths(const QStringList &paths)
{
    QSettings().setValue(kAssetsKey, paths);
}

void AssetsPanel::refresh()
{
    m_list->clear();
    for (const QString &path : storedPaths()) {
        auto *item = new QListWidgetItem(QFileInfo(path).fileName(), m_list);
        item->setToolTip(path);
        item->setData(kPathRole, path);
    }
}

void AssetsPanel::addFiles()
{
    const QStringList picked = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add assets"), QString(),
        QStringLiteral("Media (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff "
                       "*.mp4 *.mov *.m4v *.webm)"));
    if (picked.isEmpty())
        return;
    QStringList paths = storedPaths();
    for (const QString &path : picked) {
        if (!paths.contains(path))
            paths.push_back(path);
    }
    storePaths(paths);
    refresh();
}

void AssetsPanel::removeSelected()
{
    QStringList paths = storedPaths();
    for (const QListWidgetItem *item : m_list->selectedItems())
        paths.removeAll(item->data(kPathRole).toString());
    storePaths(paths);
    refresh();
}

BuilderPanel::BuilderPanel(const theme::ThemeTable &theme,
                           render::NodeLayerItem *layer,
                           const QString &templatesDirectory, QWidget *parent)
    : RailPanel(QStringLiteral("Builder"), theme, parent)
    , m_layer(layer)
    , m_directory(templatesDirectory)
{
    auto *layout = static_cast<QVBoxLayout *>(body()->layout());

    auto *hint = new QLabel(
        QStringLiteral("Double-click a template to drop it on the canvas "
                       "as one unit."),
        body());
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_list = new QListWidget(body());
    layout->addWidget(m_list, 1);

    auto *save =
        new QPushButton(QStringLiteral("Save selection as template…"), body());
    layout->addWidget(save);

    connect(save, &QPushButton::clicked, this, [this] { saveSelection(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { activate(item); });

    refresh();
}

void BuilderPanel::refresh()
{
    m_list->clear();

    auto *builtinSection =
        new QListWidgetItem(QStringLiteral("Starters"), m_list);
    builtinSection->setFlags(Qt::NoItemFlags);
    const QVector<BuiltinTemplate> builtins = builtinTemplates();
    for (int i = 0; i < builtins.size(); ++i) {
        auto *item = new QListWidgetItem(builtins[i].name, m_list);
        item->setData(kTemplateRole, i);
    }

    const QStringList files = QDir(m_directory).entryList(
        { QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    if (!files.isEmpty()) {
        auto *section = new QListWidgetItem(QStringLiteral("Saved"), m_list);
        section->setFlags(Qt::NoItemFlags);
        for (const QString &file : files) {
            auto *item =
                new QListWidgetItem(QFileInfo(file).completeBaseName(), m_list);
            item->setData(kPathRole, m_directory + QLatin1Char('/') + file);
        }
    }
}

void BuilderPanel::activate(QListWidgetItem *item)
{
    const QVariant builtinIndex = item->data(kTemplateRole);
    if (builtinIndex.isValid()) {
        const QVector<BuiltinTemplate> builtins = builtinTemplates();
        const BuiltinTemplate &chosen = builtins[builtinIndex.toInt()];
        emit templateChosen(chosen.prototypes, chosen.indexWires);
        return;
    }

    const QString path = item->data(kPathRole).toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    QVector<core::Node> prototypes;
    QVector<core::Connection> indexWires;
    if (document.isObject()
        && templateFromJson(document.object(), prototypes, indexWires, nullptr))
        emit templateChosen(prototypes, indexWires);
}

void BuilderPanel::saveSelection()
{
    QVector<core::Node> prototypes;
    QVector<core::Connection> indexWires;
    captureSelection(m_layer->graph(), prototypes, indexWires);
    if (prototypes.isEmpty())
        return;

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Save template"),
        QStringLiteral("Template name"), QLineEdit::Normal,
        QStringLiteral("My template"), &accepted);
    if (!accepted || name.trimmed().isEmpty())
        return;

    QDir().mkpath(m_directory);
    QString fileName = name.trimmed();
    fileName.replace(QLatin1Char('/'), QLatin1Char('-'));
    QSaveFile file(m_directory + QLatin1Char('/') + fileName
                   + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(templateToJson(prototypes, indexWires, name))
                   .toJson(QJsonDocument::Compact));
    file.commit();
    refresh();
}

} // namespace cutpilot::app
