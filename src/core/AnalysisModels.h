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

struct UnusedCandidateInfo
{
    int nodeIndex = -1;
    int incomingXmlReferences = 0;
    int scriptReferences = 0;
    bool whitelisted = false;
    bool protectedObject = false;
    CandidateState state = CandidateState::Blocked;
    QString reason;
    QString riskLevel;
};

struct AnalysisResult
{
    QString rootFolder;
    QVector<ScannedFileInfo> scannedFiles;
    QVector<DataNode> nodes;
    QVector<ParseErrorInfo> parseErrors;
    QHash<QString, QString> sourceXmlByFile;
    QVector<DuplicateIdGroup> duplicateIdGroups;
    QVector<DuplicateContentGroup> duplicateContentGroups;
    QVector<int> suspiciousEmptyNodeIndices;
    QVector<int> possibleUnusedNodeIndices;
    QVector<UnusedCandidateInfo> unusedCandidates;
    QString analysisReportText;
    QString plannedChangesReportText;

    int totalFilesScanned() const { return scannedFiles.size(); }
    int totalXmlFiles() const;
    int totalDataNodes() const { return nodes.size(); }
};
