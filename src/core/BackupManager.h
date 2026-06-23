#pragma once

#include <QStringList>
#include <QString>

class BackupManager
{
public:
    bool createBackup(const QString &filePath, QString *backupPath, QString *errorMessage) const;
    bool createFolderBackup(const QString &rootFolder,
                            const QStringList &relativeFilesToCopy,
                            const QString &analysisReportText,
                            const QString &plannedChangesText,
                            QString *backupFolder,
                            QString *errorMessage) const;
};
