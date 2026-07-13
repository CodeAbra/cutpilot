#include "ThemeController.h"

#include <QSettings>

namespace cutpilot::app {

namespace {

const QLatin1String kThemeKey("appearance/theme");

theme::Theme storedTheme()
{
    // CUTPILOT_THEME forces a theme for this launch without touching the
    // stored choice.
    QString value = qEnvironmentVariable("CUTPILOT_THEME");
    if (value.isEmpty())
        value = QSettings().value(kThemeKey).toString();
    if (value == QLatin1String("light"))
        return theme::Theme::Light;
    if (value == QLatin1String("dark-dim"))
        return theme::Theme::DarkDim;
    return theme::Theme::Dark;
}

QString themeKey(theme::Theme themeId)
{
    switch (themeId) {
    case theme::Theme::Light:
        return QStringLiteral("light");
    case theme::Theme::DarkDim:
        return QStringLiteral("dark-dim");
    case theme::Theme::Dark:
        break;
    }
    return QStringLiteral("dark");
}

} // namespace

ThemeController::ThemeController(QObject *parent)
    : QObject(parent)
    , m_table(storedTheme())
{
}

void ThemeController::setTheme(theme::Theme themeId)
{
    if (m_table.theme() == themeId)
        return;
    m_table.setTheme(themeId);
    QSettings().setValue(kThemeKey, themeKey(themeId));
    emit themeChanged();
}

void ThemeController::cycle()
{
    switch (m_table.theme()) {
    case theme::Theme::Dark:
        setTheme(theme::Theme::Light);
        break;
    case theme::Theme::Light:
        setTheme(theme::Theme::DarkDim);
        break;
    case theme::Theme::DarkDim:
        setTheme(theme::Theme::Dark);
        break;
    }
}

QString ThemeController::themeName() const
{
    switch (m_table.theme()) {
    case theme::Theme::Light:
        return QStringLiteral("Light");
    case theme::Theme::DarkDim:
        return QStringLiteral("Dark Dim");
    case theme::Theme::Dark:
        break;
    }
    return QStringLiteral("Dark");
}

} // namespace cutpilot::app
