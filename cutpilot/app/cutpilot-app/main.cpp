#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/CanvasItem.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWidget>

using cutpilot::render::CanvasController;
using cutpilot::render::CanvasItem;
using cutpilot::render::NodeLayerItem;
using cutpilot::theme::ThemeTable;

namespace {

// A frameless live readout pinned to the bottom-left of the canvas, mirroring the
// design's bottom-left cluster. It shows the current zoom as a percentage in the
// monospace numeric style.
class ZoomReadout : public QLabel {
public:
    explicit ZoomReadout(const ThemeTable &theme, QWidget *parent)
        : QLabel(parent)
    {
        QFont mono;
        mono.setFamilies({ QStringLiteral("SF Mono"),
                           QStringLiteral("Menlo"),
                           QStringLiteral("Cascadia Code") });
        mono.setStyleHint(QFont::Monospace);
        mono.setPixelSize(12);
        setFont(mono);

        const QColor text = theme.textSecondary();
        const QColor surface = theme.bgCanvas().lighter(125);
        const QColor border = theme.borderSubtle();
        setStyleSheet(QStringLiteral(
                          "QLabel {"
                          "  color: %1;"
                          "  background-color: rgba(%2,%3,%4,200);"
                          "  border: 1px solid %5;"
                          "  border-radius: 4px;"
                          "  padding: 3px 8px;"
                          "}")
                          .arg(text.name())
                          .arg(surface.red())
                          .arg(surface.green())
                          .arg(surface.blue())
                          .arg(border.name()));
        setText(QStringLiteral("100%"));
        adjustSize();
    }

    void setZoomPercent(qreal percent)
    {
        setText(QStringLiteral("%1%").arg(qRound(percent)));
        adjustSize();
    }
};

// Hosts the GPU canvas (grid + node layers, sharing one camera) and overlays the
// zoom readout in the bottom-left corner.
class CanvasView : public QWidget {
public:
    explicit CanvasView(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_theme(cutpilot::theme::Theme::Dark)
    {
        qmlRegisterType<CanvasController>("CutPilot.Render", 1, 0, "CanvasController");
        qmlRegisterType<CanvasItem>("CutPilot.Render", 1, 0, "CanvasItem");
        qmlRegisterType<NodeLayerItem>("CutPilot.Render", 1, 0, "NodeLayerItem");

        m_quick = new QQuickWidget(this);
        m_quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
        m_quick->setSource(QUrl(QStringLiteral("qrc:/cutpilot/app/Canvas.qml")));

        QQuickItem *root = m_quick->rootObject();
        if (root) {
            m_controller = root->findChild<CanvasController *>();
            if (auto *layer = root->findChild<NodeLayerItem *>()) {
                // Setting CUTPILOT_STRESS_NODES to a positive count seeds a wide stress
                // board for the frame-budget check; otherwise the single starter node.
                bool ok = false;
                const int stressCount =
                    qEnvironmentVariableIntValue("CUTPILOT_STRESS_NODES", &ok);
                if (ok && stressCount > 0)
                    layer->seedStressBoard(stressCount);
                else
                    layer->seedStarterNode();
            }
        }

        m_readout = new ZoomReadout(m_theme, this);

        if (m_controller) {
            QObject::connect(m_controller, &CanvasController::cameraChanged, this,
                             [this] {
                                 m_readout->setZoomPercent(m_controller->zoomPercent());
                             });
            m_readout->setZoomPercent(m_controller->zoomPercent());
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_quick->setGeometry(rect());
        const int margin = 12;
        m_readout->move(margin,
                        height() - m_readout->height() - margin);
        m_readout->raise();
    }

private:
    ThemeTable m_theme;
    QQuickWidget *m_quick = nullptr;
    CanvasController *m_controller = nullptr;
    ZoomReadout *m_readout = nullptr;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("CutPilot"));
    app.setOrganizationName(QStringLiteral("CutPilot"));

    const ThemeTable theme(cutpilot::theme::Theme::Dark);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("CutPilot"));

    auto *view = new CanvasView(&window);
    view->setStyleSheet(QStringLiteral("background-color: %1;")
                            .arg(theme.bgCanvas().name()));
    window.setCentralWidget(view);
    window.resize(1280, 800);
    window.show();

    return app.exec();
}
