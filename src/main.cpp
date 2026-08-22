#include "app/MainWindow.h"
#include "app/AudioManager.h"
#include "app/AppSettings.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QIcon>
#include <QPixmap>
#include <QSettings>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QCoreApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    QCoreApplication::setOrganizationName(QStringLiteral("SC2DataHelper"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("local"));
    sc2dh::app::AppSettings::configureRendererBeforeApplication();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    QApplication::setApplicationVersion(QStringLiteral("2.1.0"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));

    QTranslator appTranslator;
    const QString languageCode = sc2dh::app::AppSettings::resolvedLanguage();
    for (const sc2dh::app::LanguageOption &language : sc2dh::app::AppSettings::supportedLanguages()) {
        if (language.code == languageCode && !language.localeName.isEmpty()) {
            if (appTranslator.load(QStringLiteral(":/i18n/SC2DataHelper_%1.qm").arg(language.localeName)))
                app.installTranslator(&appTranslator);
            break;
        }
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
        AudioManager::instance()->shutdown();
    });

    if (sc2dh::app::AppSettings::customCursor()) {
        const QPixmap cursorPixmap(QStringLiteral(":/cursors/cursor.png"));
        if (!cursorPixmap.isNull()) QApplication::setOverrideCursor(QCursor(cursorPixmap, 2, 2));
    }

    MainWindow window;
    const QStringList args = app.arguments();
    const int wizardApplyTestIndex = args.indexOf(QStringLiteral("--wizard-apply-test"));
    if (wizardApplyTestIndex >= 0 && wizardApplyTestIndex + 1 < args.size()) {
        QString logPath;
        int timeoutMs = 600000;
        const int logIndex = args.indexOf(QStringLiteral("--wizard-apply-test-log"));
        if (logIndex >= 0 && logIndex + 1 < args.size())
            logPath = args.at(logIndex + 1);
        const int timeoutIndex = args.indexOf(QStringLiteral("--wizard-apply-timeout-ms"));
        if (timeoutIndex >= 0 && timeoutIndex + 1 < args.size()) {
            bool ok = false;
            const int parsed = args.at(timeoutIndex + 1).toInt(&ok);
            if (ok && parsed > 0)
                timeoutMs = parsed;
        }
        window.showMaximized();
        window.runWizardApplyAutomation(args.at(wizardApplyTestIndex + 1), logPath, timeoutMs);
        return app.exec();
    }

    QSettings settings;
    if (settings.value(QStringLiteral("ui/startFullscreen"), true).toBool())
        window.showFullScreen();
    else
        window.showMaximized();

    return app.exec();
}
