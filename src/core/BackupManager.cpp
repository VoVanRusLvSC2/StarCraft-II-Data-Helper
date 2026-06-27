#include "core/BackupManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace {

bool persistentBackupsEnabled()
{
    return QSettings().value(QStringLiteral("backup/enabled"), true).toBool();
}

}

bool BackupManager::createBackup(const QString &filePath, QString *backupPath, QString *errorMessage) const
{
    const QFileInfo info(filePath);
    if (!info.exists()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("File does not exist: %1").arg(filePath);
        }
        return false;
    }
    if (!persistentBackupsEnabled()) {
        if (backupPath)
            *backupPath = QStringLiteral("disabled in Settings");
        return true;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".bak-") + stamp + QLatin1Char('.') + info.suffix();
    int counter = 1;
    while (QFile::exists(candidate)) {
        candidate = info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".bak-") + stamp + QStringLiteral("-") + QString::number(counter++) + QLatin1Char('.') + info.suffix();
    }

    if (!QFile::copy(filePath, candidate)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create backup: %1").arg(candidate);
        }
        return false;
    }

    if (backupPath) {
        *backupPath = candidate;
    }
    return true;
}

bool BackupManager::createFolderBackup(const QString &rootFolder,
                                       const QStringList &relativeFilesToCopy,
                                       const QString &analysisReportText,
                                       const QString &plannedChangesText,
                                       QString *backupFolder,
                                       QString *errorMessage) const
{
    Q_UNUSED(analysisReportText);
    Q_UNUSED(plannedChangesText);

    const QFileInfo rootInfo(rootFolder);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Folder does not exist: %1").arg(rootFolder);
        }
        return false;
    }
    if (!persistentBackupsEnabled()) {
        if (backupFolder)
            *backupFolder = QStringLiteral("disabled in Settings");
        return true;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    QString backupRoot = rootInfo.absoluteFilePath() + QLatin1Char('/') + QStringLiteral("backup_") + stamp;
    int counter = 1;
    while (QFileInfo::exists(backupRoot)) {
        backupRoot = rootInfo.absoluteFilePath() + QLatin1Char('/') + QStringLiteral("backup_") + stamp + QLatin1Char('-') + QString::number(counter++);
    }
    QDir backupDir;
    if (!backupDir.mkpath(backupRoot)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create backup folder: %1").arg(backupRoot);
        }
        return false;
    }

    for (const QString &relativeFile : relativeFilesToCopy) {
        const QString sourcePath = QDir(rootFolder).absoluteFilePath(relativeFile);
        const QString targetPath = QDir(backupRoot).absoluteFilePath(relativeFile);
        QDir().mkpath(QFileInfo(targetPath).absolutePath());
        if (!QFile::copy(sourcePath, targetPath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to copy backup file: %1").arg(sourcePath);
            }
            return false;
        }
    }

    if (backupFolder) {
        *backupFolder = backupRoot;
    }
    return true;
}
