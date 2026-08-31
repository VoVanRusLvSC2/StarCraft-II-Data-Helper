#pragma once

#include "core/DataNode.h"

#include <QHash>
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

enum class AnalysisCompleteness
{
    Complete,
    Partial,
    Unknown
};

enum class RemovalSafety
{
    Safe,
    Unsafe,
    BlockedIncompleteAnalysis,
    Unknown
};

enum class OperationErrorCode
{
    None,
    ParseFailed,
    AnalysisIncomplete,
    SourceChanged,
    BackupFailed,
    SerializationFailed,
    ValidationFailed,
    SaveFailed,
    AtomicReplaceFailed
};

enum class OperationOutcome
{
    Succeeded,
    Failed,
    Cancelled
};

struct OperationResult
{
    OperationOutcome outcome = OperationOutcome::Failed;
    OperationErrorCode errorCode = OperationErrorCode::None;
    QString title;
    QString summary;
    int selected = 0;
    int applied = 0;
    int skipped = 0;
    int blocked = 0;
    QString backupPath;
    QString outputPath;
    QString temporaryOutputPath;
    bool originalChanged = false;
    QString error;
    QStringList skippedReasons;
    QStringList blockedReasons;
    QStringList details;
};

struct SourceRevision
{
    QString filePath;
    qint64 size = -1;
    QDateTime lastModifiedUtc;
    QByteArray sha256;
};

struct DestructiveOperationPermission
{
    bool allowed = false;
    OperationErrorCode errorCode = OperationErrorCode::AnalysisIncomplete;
    QString reason;
    QStringList details;
};

struct ScannedFileInfo
{
    QString filePath;
    bool isXml = false;
    bool isSc2DataLike = false;
    qint64 size = 0;
};

struct ParseErrorInfo
{
    QString filePath;
    QString message;
};

struct DuplicateIdGroup
{
    QString id;
    QVector<int> nodeIndices;
    bool sameFile = false;
    bool crossFile = false;
};

struct DuplicateContentGroup
{
    QString elementName;
    QString contentHash;
    QVector<int> nodeIndices;
    QString commonIdMask;
    bool mergeCandidate = true;
    // Exact matching XML does not prove that distinct catalog IDs are
    // interchangeable.  This is deliberately separate from manual merge
    // eligibility: only explicitly proven cases may be preselected.
    bool autoRecommended = false;
};

enum class CandidateState { Safe, Risky, Blocked };
enum class UsageState { Used, Disconnected, UnusedSubgraph, Risky, Blocked };
enum class DeepCleanupKind {
    UnusedAsset,
    LocalizationEntry,
    RedundantDefaultField,
    RedundantDefaultNode,
    BrokenActorEvent,
    DependencyEntry,
    ArchiveTrash,
    AssetAudit,
    TriggerPerformance,
    NearDuplicateObject
};
enum class DeepCleanupAction {
    DeleteFile,
    RemoveTextLine,
    RemoveXmlNode,
    RemoveXmlAttribute,
    ReportOnly
};

struct UnusedCandidateInfo
{
    int nodeIndex = -1;
    int incomingXmlReferences = 0;
    int dataCollectionReferences = 0;
    int scriptReferences = 0;
    bool whitelisted = false;
    bool protectedObject = false;
    CandidateState state = CandidateState::Blocked;
    RemovalSafety removalSafety = RemovalSafety::Unknown;
    UsageState usageState = UsageState::Blocked;
    QString reason;
    QString riskLevel;
    QStringList incomingXmlSources;
    QStringList outgoingXmlTargets;
    QStringList dataCollectionMemberships;
    QStringList externalReferenceSources;
    QStringList usagePath;
};

struct DeepCleanupCandidate
{
    int index = -1;
    DeepCleanupKind kind = DeepCleanupKind::UnusedAsset;
    DeepCleanupAction action = DeepCleanupAction::ReportOnly;
    CandidateState state = CandidateState::Risky;
    QString filePath;
    QString label;
    QString reason;
    QString detail;
    QString xmlLocation;
    QString attributeName;
    int lineNumber = -1;
    qint64 bytes = 0;
    bool recommended = false;
};

struct AnalysisResult
{
    QString rootFolder;
    // A standalone SC2Mod may expose catalog IDs and assets to maps or extension
    // mods that are not part of this analysis. Absence of a local reference is
    // not proof that an exported object or asset is unused.
    bool externalConsumersUnknown = false;
    AnalysisCompleteness completeness = AnalysisCompleteness::Unknown;
    bool sourceDiscoveryComplete = false;
    bool referenceExtractionComplete = false;
    bool dependencyGraphComplete = false;
    bool sourceChangedDuringAnalysis = false;
    QStringList unreadableSources;
    QStringList unsupportedSources;
    QStringList incompleteSources;
    QVector<SourceRevision> sourceRevisions;
    QVector<ScannedFileInfo> scannedFiles;
    QVector<DataNode> nodes;
    QVector<ParseErrorInfo> parseErrors;
    QHash<QString, QString> sourceXmlByFile;
    QVector<DuplicateIdGroup> duplicateIdGroups;
    QVector<DuplicateContentGroup> duplicateContentGroups;
    QVector<int> suspiciousEmptyNodeIndices;
    QVector<int> possibleUnusedNodeIndices;
    QVector<UnusedCandidateInfo> unusedCandidates;
    QVector<DeepCleanupCandidate> deepCleanupCandidates;
    QString analysisReportText;
    QString plannedChangesReportText;

    int totalFilesScanned() const { return scannedFiles.size(); }
    int totalXmlFiles() const;
    int totalDataNodes() const { return nodes.size(); }
};

QString analysisCompletenessName(AnalysisCompleteness completeness);
QString operationErrorCodeName(OperationErrorCode errorCode);
QString destructiveOperationPermissionText(const DestructiveOperationPermission &permission);
SourceRevision captureSourceRevision(const QString &filePath, QString *errorMessage = nullptr);
bool sourceRevisionMatches(const SourceRevision &revision, QString *reason = nullptr);
void updateAnalysisCompleteness(AnalysisResult *result);
void enforceAnalysisCompletenessSafety(AnalysisResult *result);
DestructiveOperationPermission canApplyDestructiveChanges(const AnalysisResult &analysis);
