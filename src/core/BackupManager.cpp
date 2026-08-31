#include "core/BackupManager.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>

namespace {

bool persistentBackupsEnabled()
{
    return QSettings().value(QStringLiteral("backup/enabled"), true).toBool();
}

QByteArray fileSha256(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to read file for verification: %1").arg(path);
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (errorMessage)
                *errorMessage = QStringLiteral("Unable to read file completely: %1").arg(path);
            return {};
        }
        hash.addData(chunk);
    }
    return hash.result();
}

bool writeVerified(const QString &path, const QByteArray &contents, QString *errorMessage)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(contents) != contents.size()
        || !file.commit()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Unable to commit file: %1").arg(path);
        return false;
    }
    QFile verify(path);
    if (!verify.open(QIODevice::ReadOnly) || verify.readAll() != contents) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Committed file did not pass byte verification: %1").arg(path);
        return false;
    }
    return true;
}

bool pathWithinRoot(const QString &rootFolder, const QString &relativePath, QString *absolutePath)
{
    if (relativePath.trimmed().isEmpty() || QDir::isAbsolutePath(relativePath))
        return false;
    const QString root = QDir::cleanPath(QFileInfo(rootFolder).absoluteFilePath()).replace('\\', '/');
    const QString candidate = QDir::cleanPath(QDir(root).absoluteFilePath(relativePath)).replace('\\', '/');
    if (candidate == root || !candidate.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))
        return false;
    if (absolutePath)
        *absolutePath = candidate;
    return true;
}

}

bool BackupManager::createBackup(const QString &filePath, QString *backupPath, QString *errorMessage,
                                 bool requirePersistentBackup) const
{
    const QFileInfo info(filePath);
    if (!info.exists()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("File does not exist: %1").arg(filePath);
        }
        return false;
    }
    if (!persistentBackupsEnabled() && !requirePersistentBackup) {
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

    QString sourceHashError;
    QString backupHashError;
    const QByteArray sourceHash = fileSha256(filePath, &sourceHashError);
    const QByteArray backupHash = fileSha256(candidate, &backupHashError);
    if (!sourceHashError.isEmpty() || !backupHashError.isEmpty()
        || sourceHash.isEmpty() || sourceHash != backupHash) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Backup verification failed for %1").arg(candidate);
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
                                       QString *errorMessage,
                                       bool requirePersistentBackup) const
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
    if (!persistentBackupsEnabled() && !requirePersistentBackup) {
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
        QString sourceHashError;
        QString targetHashError;
        const QByteArray sourceHash = fileSha256(sourcePath, &sourceHashError);
        const QByteArray targetHash = fileSha256(targetPath, &targetHashError);
        if (!sourceHashError.isEmpty() || !targetHashError.isEmpty()
            || sourceHash.isEmpty() || sourceHash != targetHash) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Backup verification failed for %1: %2 %3")
                                    .arg(sourcePath, sourceHashError, targetHashError).trimmed();
            }
            return false;
        }
    }

    if (backupFolder) {
        *backupFolder = backupRoot;
    }
    return true;
}

FolderSaveTransactionResult BackupManager::applyFolderTransaction(
    const QString &rootFolder,
    const QVector<TransactionalFileChange> &changes,
    const QString &analysisReportText,
    const QString &plannedChangesText,
    const StagedValidator &stagedValidator,
    const CommittedValidator &committedValidator,
    const QString &failureInjectionStep) const
{
    FolderSaveTransactionResult result;
    if (changes.isEmpty()) {
        result.errorCode = OperationErrorCode::ValidationFailed;
        result.error = QStringLiteral("Save transaction contains no changes.");
        return result;
    }

    struct ResolvedChange {
        TransactionalFileChange change;
        QString absolutePath;
        bool existed = false;
        QByteArray originalHash;
    };
    QVector<ResolvedChange> resolved;
    QSet<QString> uniquePaths;
    QStringList existingFiles;
    for (const TransactionalFileChange &change : changes) {
        QString absolutePath;
        if (!pathWithinRoot(rootFolder, change.relativePath, &absolutePath)) {
            result.errorCode = OperationErrorCode::ValidationFailed;
            result.error = QStringLiteral("Unsafe transaction path: %1").arg(change.relativePath);
            return result;
        }
        const QString pathKey = absolutePath.toCaseFolded();
        if (uniquePaths.contains(pathKey)) {
            result.errorCode = OperationErrorCode::ValidationFailed;
            result.error = QStringLiteral("Duplicate transaction path: %1").arg(change.relativePath);
            return result;
        }
        uniquePaths.insert(pathKey);
        ResolvedChange item;
        item.change = change;
        item.absolutePath = absolutePath;
        item.existed = QFileInfo::exists(absolutePath);
        if (change.remove && !item.existed)
            continue;
        if (item.existed) {
            QString hashError;
            item.originalHash = fileSha256(absolutePath, &hashError);
            if (!hashError.isEmpty() || item.originalHash.isEmpty()) {
                result.errorCode = OperationErrorCode::BackupFailed;
                result.error = hashError;
                return result;
            }
            existingFiles << change.relativePath;
        }
        resolved << item;
    }
    if (resolved.isEmpty()) {
        result.success = true;
        result.originalStateVerified = true;
        return result;
    }

    const auto verifyOriginalState = [&](QString *verificationError) {
        QStringList differences;
        for (const ResolvedChange &item : resolved) {
            if (item.existed) {
                QString hashError;
                const QByteArray currentHash = fileSha256(item.absolutePath, &hashError);
                if (!hashError.isEmpty() || currentHash.isEmpty()) {
                    differences << QStringLiteral("Cannot verify source file %1%2")
                                       .arg(item.change.relativePath,
                                            hashError.isEmpty() ? QString() : QStringLiteral(": ") + hashError);
                } else if (currentHash != item.originalHash) {
                    differences << QStringLiteral("Source file changed: %1").arg(item.change.relativePath);
                }
            } else if (QFileInfo::exists(item.absolutePath)) {
                differences << QStringLiteral("New source file appeared: %1").arg(item.change.relativePath);
            }
        }
        if (differences.isEmpty())
            return true;
        if (verificationError)
            *verificationError = differences.join(QStringLiteral("; "));
        return false;
    };

    const auto verifyNoCommitState = [&]() {
        QString verificationError;
        result.originalStateVerified = verifyOriginalState(&verificationError);
        if (!result.originalStateVerified && !verificationError.isEmpty())
            result.error += QStringLiteral("\nOriginal-state verification: %1").arg(verificationError);
    };

    QString preBackupVerificationError;
    if (!verifyOriginalState(&preBackupVerificationError)) {
        result.errorCode = OperationErrorCode::ValidationFailed;
        result.error = QStringLiteral("Source changed before save backup: %1").arg(preBackupVerificationError);
        result.originalStateVerified = false;
        return result;
    }

    if (!createFolderBackup(rootFolder, existingFiles, analysisReportText, plannedChangesText,
                            &result.backupFolder, &result.error, true)) {
        result.errorCode = OperationErrorCode::BackupFailed;
        verifyNoCommitState();
        return result;
    }
    if (failureInjectionStep == QStringLiteral("after-backup")) {
        result.errorCode = OperationErrorCode::BackupFailed;
        result.error = QStringLiteral("Injected failure after backup.");
        verifyNoCommitState();
        return result;
    }

    QTemporaryDir staging(QDir::tempPath() + QStringLiteral("/sc2dh-save-XXXXXX"));
    if (!staging.isValid()) {
        result.errorCode = OperationErrorCode::SerializationFailed;
        result.error = QStringLiteral("Unable to create save staging directory.");
        verifyNoCommitState();
        return result;
    }
    result.stagingFolder = staging.path();
    for (const ResolvedChange &item : resolved) {
        if (item.change.remove)
            continue;
        const QString stagedPath = QDir(staging.path()).absoluteFilePath(item.change.relativePath);
        if (!writeVerified(stagedPath, item.change.contents, &result.error)) {
            result.errorCode = OperationErrorCode::SerializationFailed;
            verifyNoCommitState();
            return result;
        }
    }
    if (stagedValidator && !stagedValidator(staging.path(), &result.error)) {
        result.errorCode = OperationErrorCode::ValidationFailed;
        verifyNoCommitState();
        return result;
    }
    if (failureInjectionStep == QStringLiteral("after-stage")) {
        result.errorCode = OperationErrorCode::ValidationFailed;
        result.error = QStringLiteral("Injected failure after staging.");
        verifyNoCommitState();
        return result;
    }

    QString preCommitVerificationError;
    if (!verifyOriginalState(&preCommitVerificationError)) {
        result.errorCode = OperationErrorCode::ValidationFailed;
        result.error = QStringLiteral("Source changed before save commit: %1").arg(preCommitVerificationError);
        result.originalStateVerified = false;
        return result;
    }

    const auto rollback = [&]() -> bool {
        result.rollbackAttempted = true;
        bool restored = true;
        QStringList rollbackErrors;
        for (const ResolvedChange &item : resolved) {
            if (item.existed) {
                const QString backupPath = QDir(result.backupFolder).absoluteFilePath(item.change.relativePath);
                QFile backup(backupPath);
                if (!backup.open(QIODevice::ReadOnly)) {
                    restored = false;
                    rollbackErrors << QStringLiteral("Cannot read backup %1").arg(backupPath);
                    continue;
                }
                QString writeError;
                if (!writeVerified(item.absolutePath, backup.readAll(), &writeError)) {
                    restored = false;
                    rollbackErrors << writeError;
                }
            } else if (QFileInfo::exists(item.absolutePath) && !QFile::remove(item.absolutePath)) {
                restored = false;
                rollbackErrors << QStringLiteral("Cannot remove newly created file %1").arg(item.absolutePath);
            }
        }
        for (const ResolvedChange &item : resolved) {
            if (item.existed) {
                QString hashError;
                const QByteArray restoredHash = fileSha256(item.absolutePath, &hashError);
                if (!hashError.isEmpty() || restoredHash != item.originalHash) {
                    restored = false;
                    rollbackErrors << QStringLiteral("Rollback verification failed for %1").arg(item.absolutePath);
                }
            } else if (QFileInfo::exists(item.absolutePath)) {
                restored = false;
                rollbackErrors << QStringLiteral("Rollback left new file %1").arg(item.absolutePath);
            }
        }
        result.originalStateVerified = restored;
        if (!rollbackErrors.isEmpty())
            result.error += QStringLiteral("\nRollback errors: %1").arg(rollbackErrors.join(QStringLiteral("; ")));
        return restored;
    };

    int commitIndex = 0;
    for (const ResolvedChange &item : resolved) {
        bool committed = false;
        if (item.change.remove) {
            committed = !QFileInfo::exists(item.absolutePath) || QFile::remove(item.absolutePath);
            if (!committed)
                result.error = QStringLiteral("Unable to remove file: %1").arg(item.absolutePath);
        } else {
            const QString stagedPath = QDir(staging.path()).absoluteFilePath(item.change.relativePath);
            QFile staged(stagedPath);
            if (staged.open(QIODevice::ReadOnly))
                committed = writeVerified(item.absolutePath, staged.readAll(), &result.error);
            else
                result.error = QStringLiteral("Unable to reopen staged file: %1").arg(stagedPath);
        }
        if (!committed) {
            result.errorCode = OperationErrorCode::AtomicReplaceFailed;
            rollback();
            return result;
        }
        if (item.change.remove)
            result.removedFiles << item.change.relativePath;
        else
            result.changedFiles << item.change.relativePath;
        if (failureInjectionStep == QStringLiteral("after-first-commit") && commitIndex == 0) {
            result.errorCode = OperationErrorCode::AtomicReplaceFailed;
            result.error = QStringLiteral("Injected failure after first commit.");
            rollback();
            return result;
        }
        ++commitIndex;
    }

    if (committedValidator && !committedValidator(&result.error)) {
        result.errorCode = OperationErrorCode::ValidationFailed;
        rollback();
        return result;
    }

    result.success = true;
    result.originalStateVerified = true;
    result.errorCode = OperationErrorCode::None;
    return result;
}
