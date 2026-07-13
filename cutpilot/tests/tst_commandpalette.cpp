#include <QtTest/QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QSignalSpy>

#include "CommandPalette.h"
#include "PaletteModel.h"
#include "cutpilot/core/NodeCatalog.h"
#include "cutpilot/theme/ThemeTable.h"

using namespace cutpilot;
using cutpilot::app::CommandPalette;
using cutpilot::app::PaletteModel;

Q_DECLARE_METATYPE(cutpilot::core::Node)

namespace {

QStringList rowTexts(const QVector<PaletteModel::Row> &rows,
                     PaletteModel::RowKind kind)
{
    QStringList texts;
    for (const PaletteModel::Row &row : rows) {
        if (row.kind == kind)
            texts.push_back(row.text);
    }
    return texts;
}

} // namespace

class CommandPaletteTest : public QObject {
    Q_OBJECT

private slots:
    void unfilteredRowsCarryTheWholeTaxonomy()
    {
        PaletteModel model;
        const auto rows = model.rows(QString());

        const QStringList nodeTitles =
            rowTexts(rows, PaletteModel::RowKind::Node);
        for (const core::CatalogEntry &entry : core::nodeCatalog())
            QVERIFY(nodeTitles.contains(entry.prototype.title));

        // Sections only appear with content, and never twice in a row.
        const QStringList sections =
            rowTexts(rows, PaletteModel::RowKind::Section);
        QVERIFY(sections.contains(QStringLiteral("Image")));
        QVERIFY(sections.contains(QStringLiteral("Compositing")));
        QVERIFY(!sections.contains(QStringLiteral("Models"))); // none set
    }

    void filterNarrowsAcrossTitleCategoryAndModels()
    {
        PaletteModel model;
        model.setModels({ { QStringLiteral("vendor/gen-x"),
                            QStringLiteral("Gen X Turbo"),
                            QStringLiteral("vendor"), true } });

        // A node title match.
        auto rows = model.rows(QStringLiteral("blend"));
        QCOMPARE(rowTexts(rows, PaletteModel::RowKind::Node),
                 QStringList{ QStringLiteral("Blend") });

        // A category match pulls the whole category.
        rows = model.rows(QStringLiteral("compositing"));
        const QStringList compositing =
            rowTexts(rows, PaletteModel::RowKind::Node);
        QVERIFY(compositing.contains(QStringLiteral("Blend")));
        QVERIFY(compositing.contains(QStringLiteral("Key")));

        // A model match yields a ready-configured generation prototype.
        rows = model.rows(QStringLiteral("turbo"));
        QCOMPARE(rows.size(), 2); // the Models section + the row
        QCOMPARE(rows.last().kind, PaletteModel::RowKind::Model);
        QCOMPARE(rows.last().prototype.kind, core::NodeKind::Generate);
        QCOMPARE(rows.last().prototype.modelId,
                 QStringLiteral("vendor/gen-x"));
    }

    void offerModeRestrictsToCompatibleTitles()
    {
        PaletteModel model;
        model.setModels({ { QStringLiteral("vendor/gen-x"),
                            QStringLiteral("Gen X Turbo"),
                            QStringLiteral("vendor"), true } });
        model.setOfferTitles(
            { QStringLiteral("Generate Image"), QStringLiteral("Blend") });

        const auto rows = model.rows(QString());
        QCOMPARE(rowTexts(rows, PaletteModel::RowKind::Node),
                 (QStringList{ QStringLiteral("Generate Image"),
                               QStringLiteral("Blend") }));
        QVERIFY(rowTexts(rows, PaletteModel::RowKind::Model).isEmpty());
        QVERIFY(rowTexts(rows, PaletteModel::RowKind::Section).isEmpty());

        model.clearOffers();
        QVERIFY(model.rows(QString()).size() > 4);
    }

    void keyboardDrivesFilterAndChoice()
    {
        theme::ThemeTable table(theme::Theme::Dark);
        QWidget host;
        host.resize(900, 700);
        host.show();
        CommandPalette palette(table, &host);
        QSignalSpy chosen(&palette, &CommandPalette::prototypeChosen);

        palette.open();
        QVERIFY(palette.isVisible());

        auto *search = palette.findChild<QLineEdit *>();
        QVERIFY(search);
        QTest::keyClicks(search, QStringLiteral("frame"));
        QTest::keyClick(search, Qt::Key_Return);

        QCOMPARE(chosen.count(), 1);
        const auto prototype =
            chosen.first().first().value<core::Node>();
        QCOMPARE(prototype.kind, core::NodeKind::Frame);
        QVERIFY(!palette.isVisible());
    }

    void offerChoiceAndDismissal()
    {
        theme::ThemeTable table(theme::Theme::Dark);
        QWidget host;
        host.resize(900, 700);
        host.show();
        CommandPalette palette(table, &host);
        QSignalSpy offers(&palette, &CommandPalette::offerChosen);
        QSignalSpy dismissed(&palette, &CommandPalette::dismissed);

        palette.openOffers(
            { QStringLiteral("Generate Image"), QStringLiteral("Blend") });
        auto *search = palette.findChild<QLineEdit *>();
        QVERIFY(search);
        QTest::keyClick(search, Qt::Key_Down);
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(offers.count(), 1);
        QCOMPARE(offers.first().first().toString(), QStringLiteral("Blend"));

        palette.openOffers({ QStringLiteral("Blend") });
        QTest::keyClick(search, Qt::Key_Escape);
        QCOMPARE(dismissed.count(), 1);
        QVERIFY(!palette.isVisible());
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    qRegisterMetaType<cutpilot::core::Node>();
    CommandPaletteTest testObject;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&testObject, argc, argv);
}

#include "tst_commandpalette.moc"
