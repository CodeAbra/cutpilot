#include "QuickPanel.h"

#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/ipc/GenerationCoordinator.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <numeric>

namespace cutpilot::app {

namespace {

constexpr int kPanelWidth = 480;
constexpr int kDefaultShortSide = 512;
constexpr QSize kDefaultOutput{ 768, 512 };
const QString kQuickTitle = QStringLiteral("Quick Generate");

QString money(double usd)
{
    return QStringLiteral("$%1").arg(usd, 0, 'f', 3);
}

// The aspect math clamps into the range the generation service accepts —
// the same bound every document seam enforces — so a picked format can
// never be refused for its size.
int evenClamped(double side)
{
    int value = int(side + 0.5);
    value += value & 1;
    return qBound(core::kOutputSideMin, value, core::kOutputSideMax);
}

// The node's effective output size: its own when set, the request default
// otherwise — mirroring how the coordinator resolves a submission.
QSize effectiveOutput(const core::Node &node)
{
    if (node.outputWidth > 0 && node.outputHeight > 0)
        return QSize(node.outputWidth, node.outputHeight);
    return kDefaultOutput;
}

QString aspectLabel(const QSize &size)
{
    const int divisor = std::gcd(size.width(), size.height());
    return QStringLiteral("%1:%2").arg(size.width() / divisor).arg(
        size.height() / divisor);
}

QToolButton *makeChip(const QString &text, const QString &tip, QWidget *parent)
{
    auto *chip = new QToolButton(parent);
    chip->setText(text);
    chip->setToolTip(tip);
    chip->setFixedHeight(24);
    chip->setFocusPolicy(Qt::TabFocus);
    chip->setPopupMode(QToolButton::InstantPopup);
    return chip;
}

} // namespace

QSize quickOutputSize(int shortSide, int aspectWidth, int aspectHeight)
{
    shortSide = qBound(core::kOutputSideMin, shortSide, core::kOutputSideMax);
    aspectWidth = qMax(1, aspectWidth);
    aspectHeight = qMax(1, aspectHeight);
    const double ratio = double(aspectWidth) / double(aspectHeight);
    if (ratio >= 1.0)
        return QSize(evenClamped(shortSide * ratio), evenClamped(shortSide));
    return QSize(evenClamped(shortSide), evenClamped(shortSide / ratio));
}

QuickPanel::QuickPanel(const theme::ThemeTable &theme,
                       render::NodeLayerItem *layer,
                       ipc::GenerationCoordinator *coordinator, QWidget *parent)
    : QWidget(parent)
    , m_layer(layer)
    , m_coordinator(coordinator)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(kPanelWidth);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(14, 10, 14, 12);
    column->setSpacing(8);

    auto *top = new QHBoxLayout;
    top->addStretch(1);
    m_close = new QToolButton(this);
    m_close->setObjectName(QStringLiteral("quickClose"));
    m_close->setText(QStringLiteral("✕"));
    m_close->setToolTip(QStringLiteral("Leave Quick Mode — the node stays on "
                                       "the canvas"));
    m_close->setFixedSize(24, 24);
    top->addWidget(m_close);
    column->addLayout(top);

    m_result = new QLabel(this);
    m_result->setObjectName(QStringLiteral("quickResult"));
    m_result->setAlignment(Qt::AlignCenter);
    m_result->hide();
    column->addWidget(m_result);

    m_prompt = new QPlainTextEdit(this);
    m_prompt->setObjectName(QStringLiteral("quickPrompt"));
    m_prompt->setPlaceholderText(
        QStringLiteral("Describe what you want to create…"));
    m_prompt->setTabChangesFocus(true);
    m_prompt->installEventFilter(this);
    column->addWidget(m_prompt);

    auto *chips = new QHBoxLayout;
    chips->setSpacing(6);
    m_formatChip = makeChip(QString(), QStringLiteral("Output resolution"), this);
    m_formatChip->setObjectName(QStringLiteral("quickFormatChip"));
    m_aspectChip = makeChip(QString(), QStringLiteral("Aspect ratio"), this);
    m_aspectChip->setObjectName(QStringLiteral("quickAspectChip"));
    m_presetsChip =
        makeChip(QStringLiteral("Presets"),
                 QStringLiteral("Apply a format and aspect together"), this);
    m_presetsChip->setObjectName(QStringLiteral("quickPresetsChip"));
    m_modelChip = makeChip(QStringLiteral("Model…"),
                           QStringLiteral("Pick the generation model"), this);
    m_modelChip->setObjectName(QStringLiteral("quickModelChip"));
    chips->addWidget(m_formatChip);
    chips->addWidget(m_aspectChip);
    chips->addWidget(m_presetsChip);
    chips->addStretch(1);
    chips->addWidget(m_modelChip);
    column->addLayout(chips);

    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("quickStatus"));
    m_status->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("quickProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedSize(90, 6);
    m_progress->hide();
    m_addKey = new QPushButton(QStringLiteral("Add key…"), this);
    m_addKey->setObjectName(QStringLiteral("quickAddKey"));
    m_addKey->hide();
    m_run = new QPushButton(QStringLiteral("▶ Run"), this);
    m_run->setObjectName(QStringLiteral("quickRun"));
    m_run->setToolTip(QStringLiteral("Generate (Cmd+Return)"));
    bottom->addWidget(m_status, 1);
    bottom->addWidget(m_progress);
    bottom->addWidget(m_addKey);
    bottom->addWidget(m_run);
    column->addLayout(bottom);

    // The format and aspect menus: fixed choices over the shared size math.
    auto *formatMenu = new QMenu(this);
    formatMenu->addAction(QStringLiteral("Auto — model default"), this,
                          [this] { applyTier(0); });
    for (int tier : { 512, 768, 1080 })
        formatMenu->addAction(QStringLiteral("%1p").arg(tier), this,
                              [this, tier] { applyTier(tier); });
    m_formatChip->setMenu(formatMenu);

    auto *aspectMenu = new QMenu(this);
    const struct {
        int w;
        int h;
    } aspects[] = { { 1, 1 }, { 3, 4 }, { 4, 3 }, { 16, 9 },
                    { 9, 16 }, { 3, 2 }, { 2, 3 } };
    for (const auto &aspect : aspects)
        aspectMenu->addAction(QStringLiteral("%1:%2").arg(aspect.w).arg(aspect.h),
                              this, [this, aspect] {
                                  applyAspect(aspect.w, aspect.h);
                              });
    m_aspectChip->setMenu(aspectMenu);

    auto *presetsMenu = new QMenu(this);
    const struct {
        const char *label;
        int shortSide;
        int w;
        int h;
    } presets[] = {
        { "Square · 1080 × 1080", 1080, 1, 1 },
        { "Poster · 1080 × 1440", 1080, 3, 4 },
        { "Widescreen · 1920 × 1080", 1080, 16, 9 },
    };
    for (const auto &preset : presets)
        presetsMenu->addAction(QString::fromLatin1(preset.label), this,
                               [this, preset] {
                                   applyPreset(preset.shortSide, preset.w,
                                               preset.h);
                               });
    m_presetsChip->setMenu(presetsMenu);

    m_modelMenu = new QMenu(this);
    m_modelChip->setMenu(m_modelMenu);
    rebuildModelMenu();

    connect(m_close, &QToolButton::clicked, this, &QuickPanel::leaveByUser);
    connect(m_run, &QPushButton::clicked, this, &QuickPanel::pressRun);
    connect(m_addKey, &QPushButton::clicked, this, [this] {
        const core::Node *node = m_layer->graph().nodeById(m_nodeId);
        if (!node)
            return;
        emit addKeyRequested(m_nodeId, providerForModel(node->modelId));
    });

    // The prompt body grows with its text, within a comfortable band.
    const auto growPrompt = [this] {
        const qreal docHeight =
            m_prompt->document()->documentLayout()->documentSize().height();
        m_prompt->setFixedHeight(qBound(72, int(docHeight) + 20, 190));
    };
    connect(m_prompt, &QPlainTextEdit::textChanged, this, growPrompt);
    growPrompt();

    connect(m_coordinator,
            &ipc::GenerationCoordinator::nodeContentChanged, this,
            [this](int nodeId) {
                if (nodeId != m_nodeId)
                    return;
                syncStatus();
                syncChips();
                const core::Node *node = m_layer->graph().nodeById(m_nodeId);
                if (node && !m_prompt->hasFocus()
                    && m_prompt->toPlainText() != node->promptText)
                    m_prompt->setPlainText(node->promptText);
            });
    connect(m_coordinator, &ipc::GenerationCoordinator::nodeMediaReady, this,
            [this](int nodeId, const QImage &image) {
                if (nodeId != m_nodeId)
                    return;
                m_resultImage = image;
                syncResult();
            });
    connect(m_coordinator, &ipc::GenerationCoordinator::modelsReady, this,
            [this] {
                rebuildModelMenu();
                syncChips();
                syncStatus();
            });
    connect(m_layer, &render::NodeLayerItem::graphMutated, this, [this] {
        if (m_nodeId == -1)
            return;
        if (!m_layer->graph().nodeById(m_nodeId)) {
            // The bound node was deleted or undone away; the surface has
            // nothing to stand for anymore.
            m_nodeId = -1;
            m_resultImage = QImage();
            if (isVisible()) {
                hide();
                emit dismissed();
            }
            return;
        }
        if (isVisible())
            syncFromNode();
    });

    if (parent)
        parent->installEventFilter(this);
    retheme(theme);
    hide();
}

void QuickPanel::retheme(const theme::ThemeTable &theme)
{
    m_theme = theme;
    setStyleSheet(
        QStringLiteral(
            "cutpilot--app--QuickPanel {"
            "  background-color: %1; border: 1px solid %2;"
            "  border-radius: 8px;"
            "}"
            "QPlainTextEdit {"
            "  color: %3; background-color: %4;"
            "  border: 1px solid %2; border-radius: 6px;"
            "  padding: 6px; font-size: 13px;"
            "}"
            "QPlainTextEdit:focus { border: 2px solid %5; }"
            "QToolButton {"
            "  color: %6; background-color: %4;"
            "  border: 1px solid %2; border-radius: 4px;"
            "  padding: 2px 10px; font-size: 12px;"
            "}"
            "QToolButton:hover { background-color: %7; color: %3; }"
            "QToolButton:focus { border: 2px solid %5; }"
            "QToolButton::menu-indicator { image: none; }"
            "QToolButton#quickClose { border: none; background: transparent; }"
            "QToolButton#quickClose:hover { background-color: %7; }"
            "QPushButton {"
            "  color: %6; background-color: %4;"
            "  border: 1px solid %2; border-radius: 4px;"
            "  padding: 4px 12px; font-size: 12px;"
            "}"
            "QPushButton:hover { background-color: %7; color: %3; }"
            "QPushButton:focus { border: 2px solid %5; }"
            "QPushButton#quickRun {"
            "  color: %8; background-color: %9; border: none;"
            "  padding: 5px 16px; font-weight: 600;"
            "}"
            "QLabel { color: %6; background: transparent; border: none;"
            "  font-size: 12px; }"
            "QProgressBar {"
            "  background-color: %4; border: none; border-radius: 3px;"
            "}"
            "QProgressBar::chunk { background-color: %10; border-radius: 3px; }")
            .arg(theme.surfaceOverlay().name(), theme.borderDefault().name(),
                 theme.textPrimary().name(), theme.surface1().name(),
                 theme.borderFocus().name(), theme.textSecondary().name(),
                 theme.surfaceHover().name(), theme.textOnEmphasis().name(),
                 theme.emphasis().name(), theme.statusRunning().name()));
    if (isVisible()) {
        syncStatus();
        syncResult();
    }
}

void QuickPanel::openAt(const QPointF &worldCentre)
{
    if (m_nodeId == -1 || !m_layer->graph().nodeById(m_nodeId)) {
        // Adopt the board's live quick node before making another, so the
        // surface never leaves duplicates behind.
        m_nodeId = -1;
        for (const core::Node &node : m_layer->graph().nodes()) {
            if (node.kind == core::NodeKind::Generate && node.title == kQuickTitle)
                m_nodeId = node.id;
        }
        if (m_nodeId == -1) {
            core::Node prototype =
                core::catalogPrototype(QStringLiteral("Generate Image"));
            prototype.title = kQuickTitle;
            // Seed the registry's default model so the node can run at once;
            // when the registry is still loading the coordinator assigns the
            // same default the moment it lands.
            if (!m_coordinator->models().isEmpty()) {
                prototype.modelId = m_coordinator->models().first().id;
                prototype.modelLabel = m_coordinator->models().first().label;
            }
            m_nodeId = m_layer->placePrototypeAt(prototype, worldCentre);
        }
        m_resultImage = m_layer->nodeMediaImage(m_nodeId);
    }
    syncFromNode();
    show();
    raise();
    reanchor();
    m_prompt->setFocus();
    emit nodeFocusRequested(m_nodeId);
}

void QuickPanel::dismiss()
{
    if (!isVisible())
        return;
    commitPrompt();
    hide();
}

void QuickPanel::commitPrompt()
{
    if (m_nodeId != -1)
        m_layer->setNodePrompt(m_nodeId, m_prompt->toPlainText());
}

void QuickPanel::applyTier(int shortSide)
{
    // Any structured edit flushes the typed prompt first, so a chip pick can
    // never race a graph refresh into discarding the field's text.
    commitPrompt();
    const core::Node *node = m_layer->graph().nodeById(m_nodeId);
    if (!node)
        return;
    if (shortSide <= 0) {
        m_layer->setNodeOutputFormat(m_nodeId, 0, 0);
    } else {
        const QSize current = effectiveOutput(*node);
        const QSize size =
            quickOutputSize(shortSide, current.width(), current.height());
        m_layer->setNodeOutputFormat(m_nodeId, size.width(), size.height());
    }
    syncChips();
}

void QuickPanel::applyAspect(int aspectWidth, int aspectHeight)
{
    commitPrompt();
    const core::Node *node = m_layer->graph().nodeById(m_nodeId);
    if (!node)
        return;
    const int tier = node->outputWidth > 0
        ? qMin(node->outputWidth, node->outputHeight)
        : kDefaultShortSide;
    const QSize size = quickOutputSize(tier, aspectWidth, aspectHeight);
    m_layer->setNodeOutputFormat(m_nodeId, size.width(), size.height());
    syncChips();
}

void QuickPanel::applyPreset(int shortSide, int aspectWidth, int aspectHeight)
{
    commitPrompt();
    if (m_nodeId == -1)
        return;
    const QSize size = quickOutputSize(shortSide, aspectWidth, aspectHeight);
    m_layer->setNodeOutputFormat(m_nodeId, size.width(), size.height());
    syncChips();
}

void QuickPanel::applyModel(const QString &modelId, const QString &modelLabel)
{
    commitPrompt();
    if (m_nodeId == -1)
        return;
    m_layer->setNodeModel(m_nodeId, modelId, modelLabel);
    syncChips();
    syncStatus();
}

bool QuickPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        reanchor();
    if (watched == m_prompt && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool submit = (key->key() == Qt::Key_Return
                             || key->key() == Qt::Key_Enter)
            && key->modifiers().testAnyFlags(Qt::ControlModifier
                                             | Qt::MetaModifier);
        if (submit) {
            pressRun();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            leaveByUser();
            return true;
        }
    }
    if (watched == m_prompt && event->type() == QEvent::FocusOut)
        commitPrompt();
    return QWidget::eventFilter(watched, event);
}

void QuickPanel::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        leaveByUser();
        return;
    }
    QWidget::keyPressEvent(event);
}

void QuickPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    reanchor();
}

void QuickPanel::leaveByUser()
{
    commitPrompt();
    hide();
    emit dismissed();
}

void QuickPanel::reanchor()
{
    if (!parentWidget())
        return;
    adjustSize();
    move((parentWidget()->width() - width()) / 2,
         qMax(12, (parentWidget()->height() - height()) * 2 / 5));
    raise();
}

void QuickPanel::rebuildModelMenu()
{
    m_modelMenu->clear();
    for (const auto &model : m_coordinator->models()) {
        QString label = model.label;
        if (model.needsKey && !model.hasKey)
            label += QStringLiteral(" — add key");
        m_modelMenu->addAction(label, this,
                               [this, id = model.id, name = model.label] {
                                   applyModel(id, name);
                               });
    }
    if (m_modelMenu->isEmpty()) {
        QAction *empty =
            m_modelMenu->addAction(QStringLiteral("Model registry loading…"));
        empty->setEnabled(false);
    }
}

void QuickPanel::syncFromNode()
{
    const core::Node *node = m_layer->graph().nodeById(m_nodeId);
    if (!node)
        return;
    if (!m_prompt->hasFocus() && m_prompt->toPlainText() != node->promptText)
        m_prompt->setPlainText(node->promptText);
    if (m_resultImage.isNull())
        m_resultImage = m_layer->nodeMediaImage(m_nodeId);
    syncChips();
    syncStatus();
    syncResult();
}

void QuickPanel::syncStatus()
{
    const core::Node *node = m_layer->graph().nodeById(m_nodeId);
    if (!node)
        return;

    QColor color = m_theme.textTertiary();
    QString text;
    bool running = false;
    bool needsKey = false;

    switch (node->runState) {
    case core::RunState::Idle:
        text = QStringLiteral("Ready");
        if (node->estimatedCostUsd >= 0.0)
            text += QStringLiteral(" · ~%1").arg(money(node->estimatedCostUsd));
        break;
    case core::RunState::Queued:
        color = m_theme.statusInfo();
        text = node->statusMessage;
        break;
    case core::RunState::Running:
        color = m_theme.statusRunning();
        text = QStringLiteral("Generating · %1%")
                   .arg(int(node->runProgress * 100.0));
        running = true;
        break;
    case core::RunState::Done:
        if (node->statusMessage == QStringLiteral("Reused")) {
            color = m_theme.statusInfo();
            text = QStringLiteral("Reused · %1 × %2")
                       .arg(node->resultWidth)
                       .arg(node->resultHeight);
        } else {
            color = m_theme.statusDone();
            text = QStringLiteral("Done · %1 × %2")
                       .arg(node->resultWidth)
                       .arg(node->resultHeight);
        }
        if (node->costUsd >= 0.0)
            text += QStringLiteral(" · %1").arg(money(node->costUsd));
        break;
    case core::RunState::Error:
        color = m_theme.statusError();
        text = node->statusMessage;
        break;
    case core::RunState::NeedsKey:
        color = m_theme.statusWarning();
        text = node->statusMessage;
        needsKey = true;
        break;
    case core::RunState::Held:
        color = m_theme.statusWarning();
        text = node->statusMessage.isEmpty() ? QStringLiteral("Held")
                                             : node->statusMessage;
        break;
    }

    m_status->setText(text);
    m_status->setStyleSheet(QStringLiteral(
        "color: %1; background: transparent; border: none; font-size: 12px;")
                                .arg(color.name()));
    m_progress->setVisible(running);
    if (running)
        m_progress->setValue(int(node->runProgress * 100.0));
    m_addKey->setVisible(needsKey);

    const bool stoppable = node->runState == core::RunState::Queued
        || node->runState == core::RunState::Running;
    m_run->setText(stoppable ? QStringLiteral("◼ Stop")
                             : QStringLiteral("▶ Run"));
}

void QuickPanel::syncChips()
{
    const core::Node *node = m_layer->graph().nodeById(m_nodeId);
    if (!node)
        return;
    const QSize output = effectiveOutput(*node);
    if (node->outputWidth > 0)
        m_formatChip->setText(
            QStringLiteral("%1p").arg(qMin(output.width(), output.height())));
    else
        m_formatChip->setText(QStringLiteral("Auto"));
    m_aspectChip->setText(aspectLabel(output));
    m_modelChip->setText(node->modelLabel.isEmpty() ? QStringLiteral("Model…")
                                                    : node->modelLabel);
}

void QuickPanel::syncResult()
{
    if (m_resultImage.isNull()) {
        m_result->hide();
        return;
    }
    const QSize bound(kPanelWidth - 28, 320);
    m_result->setPixmap(QPixmap::fromImage(m_resultImage.scaled(
        bound, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    m_result->show();
    reanchor();
}

void QuickPanel::pressRun()
{
    const core::Node *node = m_layer->graph().nodeById(m_nodeId);
    if (!node)
        return;
    if (node->runState == core::RunState::Queued
        || node->runState == core::RunState::Running) {
        m_coordinator->stopNode(m_nodeId);
        return;
    }
    commitPrompt();
    m_coordinator->runNode(m_nodeId);
}

QString QuickPanel::providerForModel(const QString &modelId) const
{
    for (const auto &model : m_coordinator->models()) {
        if (model.id == modelId)
            return model.provider;
    }
    return QString();
}

} // namespace cutpilot::app
