#pragma once

#include "core/AnalysisModels.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

struct DeepCleanupApplyResult
{
    bool success = false;
    QString error;
    QString backupFolder;
    int filesDeleted = 0;
    int textLinesRemoved = 0;
    int xmlNodesRemoved = 0;
    int xmlAttributesRemoved = 0;
    int reportOnlySkipped = 0;
    QStringList changedFiles;
    QStringList removedFiles;
};

QString deepCleanupKindName(DeepCleanupKind kind);
QString deepCleanupActionName(DeepCleanupAction action);

class DeepCleanupService
{
public:
    void populateCandidates(AnalysisResult *analysis) const;

    DeepCleanupApplyResult apply(const AnalysisResult &analysis,
                                 const QVector<int> &candidateIndexes,
                                 const QString &rootFolder,
                                 bool createBackup = true) const;
};
