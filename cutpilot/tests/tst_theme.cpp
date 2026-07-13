#include <QtTest/QtTest>

#include "cutpilot/theme/ThemeTable.h"

using cutpilot::theme::Theme;
using cutpilot::theme::ThemeTable;

namespace {

// WCAG relative luminance, for contrast assertions on the emphasis pairing.
qreal relativeLuminance(const QColor &color)
{
    const auto channel = [](qreal c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
        + 0.0722 * channel(color.blueF());
}

qreal contrastRatio(const QColor &a, const QColor &b)
{
    const qreal la = relativeLuminance(a);
    const qreal lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QVector<QColor> allTokens(const ThemeTable &t)
{
    return { t.bgCanvas(),       t.gridDot(),        t.gridDotMajor(),
             t.borderSubtle(),   t.borderDefault(),  t.borderStrong(),
             t.borderFocus(),    t.textPrimary(),    t.textSecondary(),
             t.textTertiary(),   t.textDisabled(),   t.textOnEmphasis(),
             t.surface0(),       t.surface1(),       t.surface2(),
             t.surface3(),       t.surfaceHover(),   t.surfaceActive(),
             t.surfaceOverlay(), t.emphasis(),       t.emphasisHover(),
             t.emphasisActive(), t.emphasisMuted(),  t.statusInfo(),
             t.nodeBody(),       t.nodeHeader(),     t.selection(),
             t.glowEmphasis(),   t.selectionFill(),  t.statusRunning(),
             t.statusDone(),     t.statusError(),    t.statusWarning() };
}

} // namespace

class ThemeTableTest : public QObject {
    Q_OBJECT

private slots:
    void everyTokenResolvesInEveryTheme()
    {
        for (Theme theme : { Theme::Dark, Theme::Light, Theme::DarkDim }) {
            const ThemeTable table(theme);
            for (const QColor &token : allTokens(table))
                QVERIFY(token.isValid());
        }
    }

    void setThemeReResolves()
    {
        ThemeTable table(Theme::Dark);
        const QColor darkSurface = table.surface2();
        table.setTheme(Theme::Light);
        QCOMPARE(table.theme(), Theme::Light);
        QVERIFY(table.surface2() != darkSurface);
        table.setTheme(Theme::DarkDim);
        QVERIFY(table.surface2() != darkSurface);
    }

    void surfacesMatchThemePolarity()
    {
        QVERIFY(relativeLuminance(ThemeTable(Theme::Dark).surface2()) < 0.5);
        QVERIFY(relativeLuminance(ThemeTable(Theme::DarkDim).surface2()) < 0.5);
        QVERIFY(relativeLuminance(ThemeTable(Theme::Light).surface2()) > 0.5);
        // Dark-Dim sits below Dark on the surface ramp.
        QVERIFY(relativeLuminance(ThemeTable(Theme::DarkDim).surface2())
                < relativeLuminance(ThemeTable(Theme::Dark).surface2()));
    }

    void emphasisLabelClearsContrastFloor()
    {
        for (Theme theme : { Theme::Dark, Theme::Light, Theme::DarkDim }) {
            const ThemeTable table(theme);
            QVERIFY(contrastRatio(table.textOnEmphasis(), table.emphasis()) >= 7.0);
        }
    }

    void bodyTextReadableOnPanels()
    {
        for (Theme theme : { Theme::Dark, Theme::Light, Theme::DarkDim }) {
            const ThemeTable table(theme);
            for (const QColor &surface :
                 { table.surface1(), table.surface2(), table.surface3() })
                QVERIFY(contrastRatio(table.textPrimary(), surface) >= 4.5);
        }
    }

    void focusRingVisibleOnChrome()
    {
        for (Theme theme : { Theme::Dark, Theme::Light, Theme::DarkDim }) {
            const ThemeTable table(theme);
            QVERIFY(contrastRatio(table.borderFocus(), table.surface2()) >= 3.0);
        }
    }
};

QTEST_APPLESS_MAIN(ThemeTableTest)
#include "tst_theme.moc"
