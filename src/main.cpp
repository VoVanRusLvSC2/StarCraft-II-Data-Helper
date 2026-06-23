#include "app/MainWindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    QApplication::setOrganizationName(QStringLiteral("SC2DataHelper"));
    QApplication::setOrganizationDomain(QStringLiteral("local"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/Icon.png")));

    MainWindow window;
    window.showFullScreen();

    return app.exec();
}
