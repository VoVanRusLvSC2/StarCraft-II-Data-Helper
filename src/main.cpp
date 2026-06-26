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
    QSettings settings;
    if (settings.value(QStringLiteral("ui/startFullscreen"), true).toBool())
        window.showFullScreen();
    else
        window.showMaximized();

    return app.exec();
}
