#include "core/ModkitMcpClient.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SC2DataHelper"));
    QCoreApplication::setApplicationName(QStringLiteral("SC2 Data Helper"));
    const QStringList arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == QStringLiteral("--configure-entry")) {
        if (!QFileInfo(arguments[2]).isFile()) return 2;
        QSettings().setValue(QStringLiteral("analysis/modkitMcpEntry"), QFileInfo(arguments[2]).absoluteFilePath());
        return 0;
    }
    if (arguments.size() < 5 || arguments.size() > 37) return 2;
    const QStringList fields = arguments.mid(5);
    const ModkitEntityInspection inspection = ModkitMcpClient::inspect(
        arguments[1], arguments[2], arguments[3], arguments[4], fields);
    const QJsonObject output{{QStringLiteral("success"), inspection.success},
                             {QStringLiteral("error"), inspection.error},
                             {QStringLiteral("input_revision"), inspection.inputRevision},
                             {QStringLiteral("answer"), inspection.answer}};
    fputs(QJsonDocument(output).toJson(QJsonDocument::Indented).constData(), stdout);
    return inspection.success ? 0 : 1;
}
