#pragma once

#include "core/DataNode.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

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
