#include "core/CatalogLinkSchema.h"
#include "core/AnalysisModels.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>

int AnalysisResult::totalXmlFiles() const
{
    int count = 0;
    for (const ScannedFileInfo &file : scannedFiles)
    {
        if (file.isXml)
        {
            ++count;
        }
    }
    return count;
}

QString analysisCompletenessName(AnalysisCompleteness completeness)
{
    switch (completeness)
    {
    case AnalysisCompleteness::Complete:
        return QStringLiteral("Complete");
    case AnalysisCompleteness::Partial:
        return QStringLiteral("Partial");
    case AnalysisCompleteness::Unknown:
        return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QString operationErrorCodeName(OperationErrorCode errorCode)
{
    switch (errorCode)
    {
    case OperationErrorCode::None:
        return QStringLiteral("none");
    case OperationErrorCode::ParseFailed:
        return QStringLiteral("parse-failed");
    case OperationErrorCode::AnalysisIncomplete:
        return QStringLiteral("analysis-incomplete");
    case OperationErrorCode::SourceChanged:
        return QStringLiteral("source-changed");
    case OperationErrorCode::BackupFailed:
        return QStringLiteral("backup-failed");
    case OperationErrorCode::SerializationFailed:
        return QStringLiteral("serialization-failed");
    case OperationErrorCode::ValidationFailed:
        return QStringLiteral("validation-failed");
    case OperationErrorCode::SaveFailed:
        return QStringLiteral("save-failed");
    case OperationErrorCode::AtomicReplaceFailed:
        return QStringLiteral("atomic-replace-failed");
    }
    return QStringLiteral("analysis-incomplete");
}

QString destructiveOperationPermissionText(const DestructiveOperationPermission &permission)
{
    QString message = QStringLiteral("[%1] %2")
                          .arg(operationErrorCodeName(permission.errorCode), permission.reason);
    if (!permission.details.isEmpty())
        message += QStringLiteral("\n") + permission.details.join(QStringLiteral("\n"));
    return message;
}

SourceRevision captureSourceRevision(const QString &filePath, QString *errorMessage)
{
    SourceRevision revision;
    revision.filePath = QFileInfo(filePath).absoluteFilePath();

    QFile file(revision.filePath);
    const QFileInfo info(file);
    revision.size = info.exists() ? info.size() : -1;
    revision.lastModifiedUtc = info.lastModified().toUTC();
    if (info.isDir())
    {
        const QString root = QDir::fromNativeSeparators(info.canonicalFilePath());
        if (info.isSymLink() || root.isEmpty())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Dependency folder is not a canonical directory: %1").arg(revision.filePath);
            return revision;
        }
        QStringList files;
        QDirIterator iterator(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext())
        {
            iterator.next();
            const QFileInfo child = iterator.fileInfo();
            const QString canonical = QDir::fromNativeSeparators(child.canonicalFilePath());
            if (child.isSymLink() || canonical.isEmpty()
                || !canonical.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)
                || (!child.isFile() && !child.isDir()))
            {
                if (errorMessage)
                    *errorMessage = QStringLiteral("Dependency folder contains an unsupported or escaping entry: %1")
                        .arg(iterator.filePath());
                return revision;
            }
            if (child.isFile()) files.append(canonical);
        }
        std::sort(files.begin(), files.end(), [&](const QString &a, const QString &b) {
            return QDir(root).relativeFilePath(a) < QDir(root).relativeFilePath(b);
        });
        QCryptographicHash folderHash(QCryptographicHash::Sha256);
        revision.size = 0;
        for (const QString &childPath : files)
        {
            QString childError;
            const SourceRevision child = captureSourceRevision(childPath, &childError);
            if (!childError.isEmpty() || child.sha256.isEmpty())
            {
                if (errorMessage)
                    *errorMessage = childError.isEmpty()
                        ? QStringLiteral("Dependency folder entry could not be hashed: %1").arg(childPath)
                        : childError;
                revision.sha256.clear();
                return revision;
            }
            const QByteArray relative = QDir(root).relativeFilePath(childPath).replace('\\', '/').toUtf8();
            folderHash.addData(relative);
            folderHash.addData("\0", 1);
            folderHash.addData(QByteArray::number(child.size));
            folderHash.addData("\0", 1);
            folderHash.addData(child.sha256);
            revision.size += child.size;
        }
        revision.sha256 = folderHash.result();
        return revision;
    }
    if (!info.exists() || !info.isFile())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Source does not exist: %1").arg(revision.filePath);
        return revision;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("Source is not readable: %1").arg(revision.filePath);
        return revision;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("Source could not be read completely: %1").arg(revision.filePath);
            revision.sha256.clear();
            return revision;
        }
        hash.addData(chunk);
    }
    revision.sha256 = hash.result();
    return revision;
}

bool sourceRevisionMatches(const SourceRevision &revision, QString *reason)
{
    QString error;
    const SourceRevision current = captureSourceRevision(revision.filePath, &error);
    if (!error.isEmpty())
    {
        if (reason)
            *reason = error;
        return false;
    }
    if (revision.sha256.isEmpty())
    {
        if (reason)
            *reason = QStringLiteral("No source hash was captured for %1.").arg(revision.filePath);
        return false;
    }
    if (current.size != revision.size || current.sha256 != revision.sha256)
    {
        if (reason)
            *reason = QStringLiteral("Source changed after analysis: %1").arg(revision.filePath);
        return false;
    }
    return true;
}

QByteArray optimizationSettingsFingerprint()
{
    QSettings settings;
    settings.sync();
    const QByteArray canonical = QByteArrayLiteral("optimization-settings-v1\nclosedProjectMode=")
        + (settings.value(QStringLiteral("optimization/closedProjectMode"), false).toBool() ? "1" : "0")
        + QByteArrayLiteral("\nbackupEnabled=")
        + (settings.value(QStringLiteral("backup/enabled"), true).toBool() ? "1" : "0");
    return QCryptographicHash::hash(canonical, QCryptographicHash::Sha256);
}

void updateAnalysisCompleteness(AnalysisResult *result)
{
    if (!result)
        return;

    const bool hasUsableAnalysis = result->sourceDiscoveryComplete
        && result->referenceExtractionComplete
        && result->dependencyGraphComplete
        && !result->scannedFiles.isEmpty();
    const bool hasIncompleteSource = !result->parseErrors.isEmpty()
        || !result->unreadableSources.isEmpty()
        || !result->unsupportedSources.isEmpty()
        || !result->incompleteSources.isEmpty()
        || result->sourceChangedDuringAnalysis;

    if (!hasUsableAnalysis)
        result->completeness = hasIncompleteSource ? AnalysisCompleteness::Partial
                                                   : AnalysisCompleteness::Unknown;
    else
        result->completeness = hasIncompleteSource ? AnalysisCompleteness::Partial
                                                   : AnalysisCompleteness::Complete;
}

void enforceAnalysisCompletenessSafety(AnalysisResult *result)
{
    if (!result || result->completeness == AnalysisCompleteness::Complete)
        return;

    result->possibleUnusedNodeIndices.clear();
    for (UnusedCandidateInfo &candidate : result->unusedCandidates)
    {
        if (candidate.state != CandidateState::Safe)
            continue;
        candidate.state = CandidateState::Blocked;
        candidate.removalSafety = RemovalSafety::BlockedIncompleteAnalysis;
        candidate.usageState = UsageState::Blocked;
        candidate.reason = QStringLiteral("Blocked: incomplete analysis. %1").arg(candidate.reason);
        candidate.riskLevel = QStringLiteral("unknown");
        if (candidate.nodeIndex >= 0 && candidate.nodeIndex < result->nodes.size())
            result->nodes[candidate.nodeIndex].candidateUnused = false;
    }
    for (DeepCleanupCandidate &candidate : result->deepCleanupCandidates)
    {
        if (candidate.state == CandidateState::Safe
            && candidate.action != DeepCleanupAction::ReportOnly)
        {
            candidate.state = CandidateState::Blocked;
            candidate.reason = QStringLiteral("Blocked: incomplete analysis. %1").arg(candidate.reason);
            candidate.recommended = false;
        }
    }
}

DestructiveOperationPermission canApplyDestructiveChanges(const AnalysisResult &analysis)
{
    DestructiveOperationPermission permission;
    if (analysis.completeness != AnalysisCompleteness::Complete)
    {
        permission.errorCode = OperationErrorCode::AnalysisIncomplete;
        permission.reason = QStringLiteral("Analysis is %1. Destructive optimization is unavailable.")
                                .arg(analysisCompletenessName(analysis.completeness));
        for (const ParseErrorInfo &error : analysis.parseErrors)
            permission.details << QStringLiteral("%1: %2").arg(error.filePath, error.message);
        permission.details += analysis.unreadableSources;
        permission.details += analysis.unsupportedSources;
        permission.details += analysis.incompleteSources;
        return permission;
    }
    if (analysis.sourceRevisions.isEmpty())
    {
        permission.errorCode = OperationErrorCode::SourceChanged;
        permission.reason = QStringLiteral("Analysis source revision is unavailable. Re-analysis is required.");
        return permission;
    }
    if (!analysis.catalogSchemaRevision.isEmpty() && analysis.catalogSchemaRevision != sc2dh::catalogSchemaFingerprint()) {
        permission.errorCode = OperationErrorCode::SourceChanged;
        permission.reason = QStringLiteral("Catalog schema or GUI type bindings changed after analysis. Re-analysis is required.");
        return permission;
    }
    if (analysis.optimizationSettingsRevision.isEmpty()
        || analysis.optimizationSettingsRevision != optimizationSettingsFingerprint()) {
        permission.errorCode = OperationErrorCode::SourceChanged;
        permission.reason = QStringLiteral("Optimization or backup settings changed after analysis. Re-analysis is required.");
        return permission;
    }
    for (const SourceRevision &revision : analysis.sourceRevisions)
    {
        QString reason;
        if (!sourceRevisionMatches(revision, &reason))
        {
            permission.errorCode = OperationErrorCode::SourceChanged;
            permission.reason = QStringLiteral("Analysis is stale. Re-analysis is required.");
            permission.details << reason;
            return permission;
        }
    }

    permission.allowed = true;
    permission.errorCode = OperationErrorCode::None;
    return permission;
}
