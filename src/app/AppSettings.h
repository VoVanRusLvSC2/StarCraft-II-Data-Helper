#pragma once

#include <QString>
#include <QVector>

namespace sc2dh::app
{
struct LanguageOption
{
    QString code;
    QString localeName;
    QString nativeName;
};

class AppSettings final
{
public:
    static QVector<LanguageOption> supportedLanguages();
    static QString selectedLanguage();
    static QString resolvedLanguage();

    static QString graphicsPreset();
    static void applyGraphicsPreset(const QString &preset);

    static bool buttonAnimations();
    static bool backgroundGlows();
    static bool decorativeTextures();
    static bool customCursor();
    static bool previewImages();
    static bool previewModels();
    static bool modelAntialiasing();
    static int previewLimitMiB();

    static QString renderer();
    static void configureRendererBeforeApplication();
};
}
