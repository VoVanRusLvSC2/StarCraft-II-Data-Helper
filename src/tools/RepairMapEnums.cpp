#include "core/CatalogEnumRepair.h"
#include "core/Sc2Archive.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QTextStream>

namespace
{

bool copyFile(const QString &source, const QString &target, QString *error)
{
    QFile::remove(target);
    if (!QFile::copy(source, target)) {
        if (error)
            *error = QStringLiteral("Unable to copy %1 to %2").arg(source, target);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList args = app.arguments();
    if (args.size() < 2 || args.size() > 3) {
        err << "Usage: SC2RepairMapEnums <map.SC2Map> [output.SC2Map]\n";
        return 2;
    }

    const QString input = QDir::fromNativeSeparators(args.at(1));
    const QString requestedOutput = args.size() == 3 ? QDir::fromNativeSeparators(args.at(2)) : input;
    const bool replaceInput = QFileInfo(input).absoluteFilePath().compare(
                                  QFileInfo(requestedOutput).absoluteFilePath(), Qt::CaseInsensitive) == 0;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString sourceForRewrite = input;
    QString outputPath = requestedOutput;
    QString backupPath;
    QString error;

    if (replaceInput) {
        backupPath = input + QStringLiteral(".bak-repair-%1.SC2Map").arg(stamp);
        if (!copyFile(input, backupPath, &error)) {
            err << error << '\n';
            return 3;
        }
        sourceForRewrite = backupPath;
        outputPath = input + QStringLiteral(".repaired-%1.SC2Map").arg(stamp);
    }

    Sc2Archive archive;
    if (!archive.load(sourceForRewrite, &error)) {
        err << "archive_load_failed=" << error << '\n';
        return 4;
    }

    QHash<QString, QByteArray> replacements;
    int changedEntries = 0;
    int totalChanges = 0;
    for (const QString &entry : archive.gameDataXmlEntries()) {
        QByteArray bytes;
        if (!archive.readEntry(entry, &bytes, &error)) {
            err << "archive_read_failed=" << entry << " | " << error << '\n';
            return 5;
        }
        int changes = 0;
        if (!sc2dh::repairKnownCatalogEnumDamage(&bytes, &changes, &error)) {
            err << "repair_failed=" << entry << " | " << error << '\n';
            return 5;
        }
        if (changes <= 0)
            continue;
        replacements.insert(entry, bytes);
        ++changedEntries;
        totalChanges += changes;
        out << "repair_entry=" << entry << " changes=" << changes << '\n';
    }

    if (replacements.isEmpty()) {
        out << "changed_entries=0 total_changes=0";
        if (!backupPath.isEmpty())
            out << " backup=" << backupPath;
        out << '\n';
        return 0;
    }

    if (!archive.saveCopy(outputPath, replacements, {}, &error)) {
        err << "archive_save_failed=" << error << '\n';
        return 6;
    }

    QString replaceStatus;
    if (replaceInput) {
        if (QFile::remove(input)) {
            if (QFile::rename(outputPath, input)) {
                outputPath = input;
                replaceStatus = QStringLiteral("replaced");
            } else {
                replaceStatus = QStringLiteral("replace_failed_repaired_copy_kept");
                QString restoreError;
                copyFile(backupPath, input, &restoreError);
            }
        } else {
            replaceStatus = QStringLiteral("target_busy_repaired_copy_kept");
        }
    }

    out << "changed_entries=" << changedEntries
        << " total_changes=" << totalChanges
        << " output=" << outputPath;
    if (!backupPath.isEmpty())
        out << " backup=" << backupPath;
    if (!replaceStatus.isEmpty())
        out << " in_place=" << replaceStatus;
    out << '\n';
    return 0;
}
