#pragma once

#include "core/AnalysisModels.h"

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QString>
#include <QVector>

#include <functional>

struct TransactionalFileChange
{
    QString relativePath;
    QByteArray contents;
    bool remove = false;
};

struct FolderSaveTransactionResult
{
    bool success = false;
    bool rollbackAttempted = false;
    bool originalStateVerified = false;
    OperationErrorCode errorCode = OperationErrorCode::None;
    QString error;
    QString backupFolder;
    QString stagingFolder;
    QStringList changedFiles;
    QStringList removedFiles;
};

class BackupManager
{
public:
    using StagedValidator = std::function<bool(const QString &stagingFolder, QString *errorMessage)>;
    using CommittedValidator = std::function<bool(QString *errorMessage)>;

    bool createBackup(const QString &filePath, QString *backupPath, QString *errorMessage,
                      bool requirePersistentBackup = false) const;
    bool createFolderBackup(const QString &rootFolder,
                            const QStringList &relativeFilesToCopy,
                            const QString &analysisReportText,
                            const QString &plannedChangesText,
                            QString *backupFolder,
                            QString *errorMessage,
                            bool requirePersistentBackup = false) const;

    FolderSaveTransactionResult applyFolderTransaction(
        const QString &rootFolder,
        const QVector<TransactionalFileChange> &changes,
        const QString &analysisReportText,
        const QString &plannedChangesText,
        const StagedValidator &stagedValidator = {},
        const CommittedValidator &committedValidator = {},
        const QString &failureInjectionStep = {}) const;
};
