#include "app/AppSettings.h"

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>

namespace sc2dh::app
{
QVector<LanguageOption> AppSettings::supportedLanguages()
{
    return {
        {QStringLiteral("system"), QString(), QStringLiteral("System language")},
        {QStringLiteral("enUS"), QStringLiteral("en_US"), QStringLiteral("English (US)")},
        {QStringLiteral("ruRU"), QStringLiteral("ru_RU"), QStringLiteral("Русский")},
        {QStringLiteral("deDE"), QStringLiteral("de_DE"), QStringLiteral("Deutsch")},
        {QStringLiteral("esMX"), QStringLiteral("es_MX"), QStringLiteral("Español (Latinoamérica)")},
        {QStringLiteral("esES"), QStringLiteral("es_ES"), QStringLiteral("Español (España)")},
        {QStringLiteral("frFR"), QStringLiteral("fr_FR"), QStringLiteral("Français")},
        {QStringLiteral("itIT"), QStringLiteral("it_IT"), QStringLiteral("Italiano")},
        {QStringLiteral("plPL"), QStringLiteral("pl_PL"), QStringLiteral("Polski")},
        {QStringLiteral("ptBR"), QStringLiteral("pt_BR"), QStringLiteral("Português (Brasil)")},
        {QStringLiteral("koKR"), QStringLiteral("ko_KR"), QStringLiteral("한국어")},
        {QStringLiteral("zhCN"), QStringLiteral("zh_CN"), QStringLiteral("简体中文")},
        {QStringLiteral("zhTW"), QStringLiteral("zh_TW"), QStringLiteral("繁體中文")}
    };
}

QString AppSettings::selectedLanguage()
{
    return QSettings().value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
}

QString AppSettings::resolvedLanguage()
{
    const QString selected = selectedLanguage();
    if (selected != QStringLiteral("system"))
        return selected;

    const QString systemName = QLocale::system().name();
    for (const LanguageOption &language : supportedLanguages()) {
        if (!language.localeName.isEmpty()
            && systemName.compare(language.localeName, Qt::CaseInsensitive) == 0)
            return language.code;
    }
    for (const LanguageOption &language : supportedLanguages()) {
        if (!language.localeName.isEmpty()
            && systemName.startsWith(language.localeName.left(2), Qt::CaseInsensitive))
            return language.code;
    }
    return QStringLiteral("enUS");
}

QString AppSettings::graphicsPreset()
{
    QSettings settings;
    if (settings.contains(QStringLiteral("graphics/preset")))
        return settings.value(QStringLiteral("graphics/preset")).toString();
    return buttonAnimations() && backgroundGlows() && decorativeTextures() && customCursor()
            && previewImages() && previewModels() && modelAntialiasing()
        ? QStringLiteral("full") : QStringLiteral("custom");
}

void AppSettings::applyGraphicsPreset(const QString &preset)
{
    QSettings settings;
    settings.setValue(QStringLiteral("graphics/preset"), preset);
    if (preset == QStringLiteral("custom"))
        return;

    const bool full = preset == QStringLiteral("full");
    const bool balanced = preset == QStringLiteral("balanced");
    settings.setValue(QStringLiteral("ui/buttonAnimations"), full || balanced);
    settings.setValue(QStringLiteral("ui/backgroundGlows"), full);
    settings.setValue(QStringLiteral("ui/decorativeTextures"), full || balanced);
    settings.setValue(QStringLiteral("ui/customCursor"), full || balanced);
    settings.setValue(QStringLiteral("preview/images"), true);
    settings.setValue(QStringLiteral("preview/models"), full || balanced);
    settings.setValue(QStringLiteral("preview/modelAntialiasing"), full);
}

bool AppSettings::buttonAnimations() { return QSettings().value(QStringLiteral("ui/buttonAnimations"), true).toBool(); }
bool AppSettings::backgroundGlows() { return QSettings().value(QStringLiteral("ui/backgroundGlows"), true).toBool(); }
bool AppSettings::decorativeTextures() { return QSettings().value(QStringLiteral("ui/decorativeTextures"), true).toBool(); }
bool AppSettings::customCursor() { return QSettings().value(QStringLiteral("ui/customCursor"), true).toBool(); }
bool AppSettings::previewImages() { return QSettings().value(QStringLiteral("preview/images"), true).toBool(); }
bool AppSettings::previewModels() { return QSettings().value(QStringLiteral("preview/models"), true).toBool(); }
bool AppSettings::modelAntialiasing() { return QSettings().value(QStringLiteral("preview/modelAntialiasing"), true).toBool(); }
int AppSettings::previewLimitMiB() { return qBound(1, QSettings().value(QStringLiteral("preview/maxFileMiB"), 64).toInt(), 512); }
QString AppSettings::renderer() { return QSettings().value(QStringLiteral("graphics/renderer"), QStringLiteral("auto")).toString(); }

void AppSettings::configureRendererBeforeApplication()
{
    const QString mode = renderer();
    if (mode == QStringLiteral("software")) {
        qputenv("QT_OPENGL", "software");
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    } else if (mode == QStringLiteral("desktop")) {
        qputenv("QT_OPENGL", "desktop");
        QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    }
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
}
}
