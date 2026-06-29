#include "app/MainWindow.h"
#include "app/AudioManager.h"

#include <QApplication>
#include <QCursor>
#include <QIcon>
#include <QPixmap>
#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    QApplication::setOrganizationName(QStringLiteral("SC2DataHelper"));
    QApplication::setOrganizationDomain(QStringLiteral("local"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
        AudioManager::instance()->shutdown();
    });

    const QPixmap cursorPixmap(QStringLiteral(":/cursors/cursor.png"));
    if (!cursorPixmap.isNull()) QApplication::setOverrideCursor(QCursor(cursorPixmap, 2, 2));

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
