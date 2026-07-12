#include "CompositeInspector.h"

#include "cutpilot/core/CompositeNodes.h"
#include "cutpilot/core/NodeGraph.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/render/PreviewController.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace cutpilot::app {

using render::NodeLayerItem;
using render::PreviewController;
using theme::ThemeTable;

CompositeInspector::CompositeInspector(const ThemeTable &theme,
                                       NodeLayerItem *layer,
                                       PreviewController *previews,
                                       QWidget *parent)
    : QWidget(parent)
    , m_layer(layer)
    , m_previews(previews)
{
    const QColor surface = theme.bgCanvas().lighter(125);
    setStyleSheet(QStringLiteral(
                      "QWidget { color: %1; }"
                      "QLabel { color: %1; background: transparent; "
                      "border: none; }"
                      "QPushButton, QComboBox {"
                      "  color: %1; background-color: rgba(%2,%3,%4,220);"
                      "  border: 1px solid %5; border-radius: 4px;"
                      "  padding: 2px 8px;"
                      "}"
                      "QCheckBox { color: %1; background: transparent; }"
                      "QSlider { background: transparent; }")
                      .arg(theme.textPrimary().name())
                      .arg(surface.red())
                      .arg(surface.green())
                      .arg(surface.blue())
                      .arg(theme.borderSubtle().name()));

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(8, 8, 8, 8);
    column->setSpacing(6);

    auto *header = new QHBoxLayout;
    m_title = new QLabel(this);
    auto *close = new QPushButton(QStringLiteral("✕"), this);
    close->setFixedWidth(26);
    header->addWidget(m_title, 1);
    header->addWidget(close);
    column->addLayout(header);

    m_controls = new QWidget(this);
    new QVBoxLayout(m_controls);
    m_controls->layout()->setContentsMargins(0, 0, 0, 0);
    column->addWidget(m_controls);

    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    connect(layer, &NodeLayerItem::graphMutated, this, [this] {
        if (m_nodeId == -1)
            return;
        const core::Node *node = m_layer->graph().nodeById(m_nodeId);
        // The edited node can vanish through delete or undo.
        if (!node) {
            if (isVisible())
                hide();
            return;
        }
        // The node's content moved without this panel writing it — an undo,
        // a redo, or another writer. The graph is the truth: re-read it, or
        // the next gesture here would push stale values back over it.
        if (node->contentRevision != m_seenRevision) {
            m_before = node->comp;
            m_current = node->comp;
            m_seenRevision = node->contentRevision;
            rebuildControls(node->kind);
        }
    });

    if (parent)
        parent->installEventFilter(this);
    setFixedWidth(280);
    hide();
}

void CompositeInspector::openFor(int nodeId)
{
    const core::Node *node = m_layer->graph().nodeById(nodeId);
    if (!node || !core::isCompositeKind(node->kind))
        return;
    m_nodeId = nodeId;
    m_before = node->comp;
    m_current = node->comp;
    m_seenRevision = node->contentRevision;
    m_title->setText(node->title);
    rebuildControls(node->kind);
    show();
    raise();
    reanchor();
}

bool CompositeInspector::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
        reanchor();
    return QWidget::eventFilter(watched, event);
}

void CompositeInspector::reanchor()
{
    if (!parentWidget())
        return;
    const int margin = 12;
    move(parentWidget()->width() - width() - margin,
         qMax(margin, parentWidget()->height() / 2 - height() / 2));
    raise();
}

void CompositeInspector::preview()
{
    m_layer->previewCompositeParams(m_nodeId, m_current);
    // This panel's own write; the resync above must not mistake it for a
    // foreign change.
    if (const core::Node *node = m_layer->graph().nodeById(m_nodeId))
        m_seenRevision = node->contentRevision;
    m_previews->refresh();
}

void CompositeInspector::commit()
{
    m_layer->commitCompositeParams(m_nodeId, m_before, m_current);
    m_before = m_current;
}

QSlider *CompositeInspector::addSlider(const QString &label, int min, int max,
                                       int value,
                                       const std::function<void(int)> &apply)
{
    auto *box = static_cast<QVBoxLayout *>(m_controls->layout());
    auto *row = new QHBoxLayout;
    auto *caption = new QLabel(label, m_controls);
    caption->setFixedWidth(70);
    auto *slider = new QSlider(Qt::Horizontal, m_controls);
    slider->setRange(min, max);
    slider->setValue(value);
    row->addWidget(caption);
    row->addWidget(slider, 1);
    box->addLayout(row);
    connect(slider, &QSlider::valueChanged, this, [this, apply](int v) {
        apply(v);
        preview();
    });
    connect(slider, &QSlider::sliderReleased, this, [this] { commit(); });
    return slider;
}

void CompositeInspector::rebuildControls(core::NodeKind kind)
{
    // Rebuild the control set for the node being edited.
    qDeleteAll(m_controls->findChildren<QWidget *>(
        Qt::FindDirectChildrenOnly));
    QLayout *box = m_controls->layout();
    while (QLayoutItem *item = box->takeAt(0)) {
        if (QLayout *nested = item->layout()) {
            while (QLayoutItem *inner = nested->takeAt(0))
                delete inner;
        }
        delete item;
    }

    switch (kind) {
    case core::NodeKind::Blend: {
        auto *modes = new QComboBox(m_controls);
        for (int i = 0; i < core::blendModeCount(); ++i)
            modes->addItem(core::blendModeLabel(core::BlendMode(i)));
        modes->setCurrentIndex(int(m_current.blendMode));
        box->addWidget(modes);
        connect(modes, &QComboBox::currentIndexChanged, this, [this](int i) {
            m_current.blendMode = core::BlendMode(i);
            preview();
            commit();
        });
        addSlider(QStringLiteral("Opacity"), 0, 100,
                  qRound(m_current.opacity * 100.0), [this](int v) {
                      m_current.opacity = v / 100.0;
                  });
        break;
    }
    case core::NodeKind::Mask: {
        auto *invert = new QCheckBox(QStringLiteral("Invert mask"),
                                     m_controls);
        invert->setChecked(m_current.invertMask);
        box->addWidget(invert);
        connect(invert, &QCheckBox::toggled, this, [this](bool on) {
            m_current.invertMask = on;
            preview();
            commit();
        });
        break;
    }
    case core::NodeKind::Key: {
        auto *modes = new QComboBox(m_controls);
        modes->addItem(QStringLiteral("Chroma"));
        modes->addItem(QStringLiteral("Luma"));
        modes->setCurrentIndex(m_current.lumaKey ? 1 : 0);
        box->addWidget(modes);
        connect(modes, &QComboBox::currentIndexChanged, this, [this](int i) {
            m_current.lumaKey = i == 1;
            preview();
            commit();
        });
        auto *color = new QPushButton(QStringLiteral("Key color…"),
                                      m_controls);
        box->addWidget(color);
        connect(color, &QPushButton::clicked, this, [this] {
            const QColor picked = QColorDialog::getColor(
                m_current.keyColor, this, QStringLiteral("Key color"));
            if (!picked.isValid())
                return;
            m_current.keyColor = picked;
            preview();
            commit();
        });
        addSlider(QStringLiteral("Tolerance"), 0, 100,
                  qRound(m_current.keyTolerance * 100.0), [this](int v) {
                      m_current.keyTolerance = v / 100.0;
                  });
        addSlider(QStringLiteral("Softness"), 0, 100,
                  qRound(m_current.keySoftness * 100.0), [this](int v) {
                      m_current.keySoftness = v / 100.0;
                  });
        break;
    }
    case core::NodeKind::Transform: {
        addSlider(QStringLiteral("X"), -100, 100,
                  qRound(m_current.translateX * 100.0), [this](int v) {
                      m_current.translateX = v / 100.0;
                  });
        addSlider(QStringLiteral("Y"), -100, 100,
                  qRound(m_current.translateY * 100.0), [this](int v) {
                      m_current.translateY = v / 100.0;
                  });
        addSlider(QStringLiteral("Scale"), 10, 400,
                  qRound(m_current.scale * 100.0), [this](int v) {
                      m_current.scale = v / 100.0;
                  });
        addSlider(QStringLiteral("Rotation"), -180, 180,
                  qRound(m_current.rotationDeg), [this](int v) {
                      m_current.rotationDeg = double(v);
                  });
        break;
    }
    default:
        break;
    }
    adjustSize();
}

} // namespace cutpilot::app
