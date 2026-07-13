#include <QtTest/QtTest>

#include <QApplication>
#include <QSignalSpy>
#include <QLineEdit>
#include <QToolButton>

#include "CanvasCluster.h"
#include "ToolPill.h"
#include "TopBar.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/render/CanvasController.h"
#include "cutpilot/render/NodeLayerItem.h"
#include "cutpilot/theme/ThemeTable.h"

using namespace cutpilot;
using cutpilot::app::CanvasCluster;
using cutpilot::app::ToolPill;
using cutpilot::app::TopBar;

namespace {

// The rig mirrors the app: a real layer and camera, chrome wired on top.
struct Rig {
    theme::ThemeTable table{ theme::Theme::Dark };
    render::CanvasController controller;
    render::NodeLayerItem layer;

    Rig()
    {
        layer.setSize(QSizeF(1600, 1000));
        layer.setController(&controller);
    }
};

QToolButton *buttonByTooltipPrefix(QWidget *root, const QString &prefix)
{
    for (QToolButton *button : root->findChildren<QToolButton *>()) {
        if (button->toolTip().startsWith(prefix))
            return button;
    }
    return nullptr;
}

} // namespace

class ChromeWiringTest : public QObject {
    Q_OBJECT

private slots:
    void pillDrivesAndMirrorsTheCanvasTool()
    {
        Rig rig;
        ToolPill pill(rig.table, &rig.layer, nullptr);

        QToolButton *cut = buttonByTooltipPrefix(&pill, QStringLiteral("Cut"));
        QToolButton *cursor =
            buttonByTooltipPrefix(&pill, QStringLiteral("Cursor"));
        QToolButton *connectTool =
            buttonByTooltipPrefix(&pill, QStringLiteral("Connect"));
        QVERIFY(cut && cursor && connectTool);
        QVERIFY(cursor->isChecked());

        cut->click();
        QCOMPARE(rig.layer.tool(), render::NodeLayerItem::Tool::Cut);
        QVERIFY(cut->isChecked());
        QVERIFY(!cursor->isChecked());

        // The pill mirrors a tool change made elsewhere (Escape, shortcuts).
        rig.layer.setTool(render::NodeLayerItem::Tool::Connect);
        QVERIFY(connectTool->isChecked());
        QVERIFY(!cut->isChecked());
        rig.layer.setTool(render::NodeLayerItem::Tool::Cursor);
        QVERIFY(cursor->isChecked());
    }

    void pillAddToolsArmPlacement()
    {
        Rig rig;
        ToolPill pill(rig.table, &rig.layer, nullptr);

        QToolButton *image =
            buttonByTooltipPrefix(&pill, QStringLiteral("Image"));
        QToolButton *frame =
            buttonByTooltipPrefix(&pill, QStringLiteral("Frame"));
        QVERIFY(image && frame);

        image->click();
        QCOMPARE(rig.layer.tool(), render::NodeLayerItem::Tool::Place);
        QVERIFY(image->isChecked());

        frame->click();
        QVERIFY(frame->isChecked());
        QVERIFY(!image->isChecked());
        QCOMPARE(rig.layer.tool(), render::NodeLayerItem::Tool::Place);
    }

    void clusterHistoryButtonsTrackAndDriveTheStack()
    {
        Rig rig;
        CanvasCluster cluster(rig.table, &rig.layer, &rig.controller, nullptr);

        QToolButton *undo =
            buttonByTooltipPrefix(&cluster, QStringLiteral("Undo"));
        QToolButton *redo =
            buttonByTooltipPrefix(&cluster, QStringLiteral("Redo"));
        QVERIFY(undo && redo);
        QVERIFY(!undo->isEnabled());
        QVERIFY(!redo->isEnabled());

        rig.layer.placePrototypeAt(
            core::catalogPrototype(QStringLiteral("Prompt")),
            QPointF(400, 300));
        QVERIFY(undo->isEnabled());
        QVERIFY(!redo->isEnabled());

        undo->click();
        QVERIFY(rig.layer.graph().nodes().isEmpty());
        QVERIFY(!undo->isEnabled());
        QVERIFY(redo->isEnabled());

        redo->click();
        QCOMPARE(rig.layer.graph().nodes().size(), 1);
    }

    void clusterZoomLabelAndFitDriveTheCamera()
    {
        Rig rig;
        CanvasCluster cluster(rig.table, &rig.layer, &rig.controller, nullptr);

        QToolButton *zoom =
            buttonByTooltipPrefix(&cluster, QStringLiteral("Zoom"));
        QVERIFY(zoom);
        QVERIFY(zoom->text().startsWith(QStringLiteral("100%")));

        rig.controller.zoomAbout(QPointF(0, 0), 2.0, 1.0);
        QVERIFY(zoom->text().startsWith(QStringLiteral("200%")));

        // Fit frames the whole board inside the layer-sized viewport.
        rig.layer.placePrototypeAt(
            core::catalogPrototype(QStringLiteral("Prompt")),
            QPointF(5000, 4000));
        rig.layer.placePrototypeAt(
            core::catalogPrototype(QStringLiteral("Prompt")),
            QPointF(-1000, -500));
        cluster.fitAll();
        const QRectF bounds = rig.layer.contentWorldBounds();
        const QPointF centre =
            rig.controller.screenFromWorld(bounds.center(), 1.0);
        QVERIFY(qAbs(centre.x() - 800.0) < 1.0);
        QVERIFY(qAbs(centre.y() - 500.0) < 1.0);
        const QPointF topLeft =
            rig.controller.screenFromWorld(bounds.topLeft(), 1.0);
        QVERIFY(topLeft.x() >= 0.0 && topLeft.y() >= 0.0);
    }

    void clusterMinimapToggleSignals()
    {
        Rig rig;
        CanvasCluster cluster(rig.table, &rig.layer, &rig.controller, nullptr);
        QSignalSpy toggles(&cluster, &CanvasCluster::minimapToggled);

        QVERIFY(cluster.minimapVisible());
        QToolButton *toggle =
            buttonByTooltipPrefix(&cluster, QStringLiteral("Show or hide"));
        QVERIFY(toggle);
        toggle->click();
        QCOMPARE(toggles.count(), 1);
        QCOMPARE(toggles.first().first().toBool(), false);
        QVERIFY(!cluster.minimapVisible());

        // The canvas's M shortcut arrives as a request the cluster serves.
        cluster.setMinimapVisible(true);
        QCOMPARE(toggles.count(), 2);
        QCOMPARE(toggles.last().first().toBool(), true);
    }

    void topBarModesAndNameCommit()
    {
        theme::ThemeTable table(theme::Theme::Dark);
        TopBar bar(table, nullptr);
        QSignalSpy modes(&bar, &TopBar::modeSelected);
        QSignalSpy names(&bar, &TopBar::workflowNameCommitted);

        QCOMPARE(TopBar::modeNames().size(), 6);
        const int nodeIndex =
            TopBar::modeNames().indexOf(QStringLiteral("Node"));
        bar.setActiveMode(nodeIndex);
        QCOMPARE(bar.activeMode(), nodeIndex);

        // Clicking another tab announces the switch and reads as active.
        QToolButton *director = nullptr;
        for (QToolButton *button : bar.findChildren<QToolButton *>()) {
            if (button->text() == QStringLiteral("Director"))
                director = button;
        }
        QVERIFY(director);
        director->click();
        QCOMPARE(modes.count(), 1);
        QCOMPARE(modes.first().first().toInt(), 0);
        QCOMPARE(bar.activeMode(), 0);
        QVERIFY(director->isChecked());

        bar.setWorkflowName(QStringLiteral("Dusk Reel"));
        QCOMPARE(bar.workflowName(), QStringLiteral("Dusk Reel"));
        auto *edit = bar.findChild<QLineEdit *>();
        QVERIFY(edit);
        edit->setFocus();
        QTest::keyClicks(edit, QStringLiteral(" II"));
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(names.count(), 1);
        QCOMPARE(names.first().first().toString(),
                 QStringLiteral("Dusk Reel II"));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    ChromeWiringTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_chromewiring.moc"
