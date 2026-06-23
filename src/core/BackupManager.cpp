#include "core/BackupManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QIODevice>

bool BackupManager::createBackup(const QString &filePath, QString *backupPath, QString *errorMessage) const
{
    const QFileInfo info(filePath);
    if (!info.exists()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("File does not exist: %1").arg(filePath);
        }
        return false;
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
    const QFileInfo rootInfo(rootFolder);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Folder does not exist: %1").arg(rootFolder);
        }
        return false;
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

    {
        QSaveFile analysisFile(QDir(backupRoot).absoluteFilePath(QStringLiteral("analysis_report.txt")));
        if (!analysisFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to write analysis report into backup.");
            }
            return false;
        }
        analysisFile.write(analysisReportText.toUtf8());
        if (!analysisFile.commit()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to commit analysis report into backup.");
            }
            return false;
        }
    }

    {
        QSaveFile plannedFile(QDir(backupRoot).absoluteFilePath(QStringLiteral("planned_changes_report.txt")));
        if (!plannedFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to write planned changes report into backup.");
            }
            return false;
        }
        plannedFile.write(plannedChangesText.toUtf8());
        if (!plannedFile.commit()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to commit planned changes report into backup.");
            }
            return false;
        }
    }

    if (backupFolder) {
        *backupFolder = backupRoot;
    }
    return true;
}
