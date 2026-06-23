#pragma once

#include "core/AnalysisModels.h"
#include "core/BackupManager.h"
#include "core/XmlLoader.h"

#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QString>
#include <functional>

class FolderAnalyzer
{
public:
    bool analyzeFolder(const QString &rootFolder,
                       const QSet<QString> &whitelistIds,
                       AnalysisResult *result,
                       QString *errorMessage,
                       const std::function<void(int, int, const QString &)> &progress = {},
                       const std::function<bool()> &isCancelled = {}) const;

    QString buildAnalysisReport(const AnalysisResult &result) const;
    QString buildDryRunReport(const AnalysisResult &result, const QVector<int> &selectedRows) const;
    QString buildPlannedChangesReport(const AnalysisResult &result, const QVector<int> &selectedRows) const;
    bool populateReferenceIds(AnalysisResult *result,
                              const std::function<void()> &heartbeat = {},
                              const std::function<bool()> &isCancelled = {}) const;

    bool applySelectedChanges(const AnalysisResult &result,
                              const QVector<int> &selectedRows,
                              const QString &rootFolder,
                              const QSet<QString> &whitelistIds,
                              QString *backupFolder,
                              QString *errorMessage,
                              QStringList *changedFiles,
                              int *removedNodes,
                              int *skippedNodes) const;

private:
    bool isXmlFile(const QFileInfo &info) const;
    bool isSc2DataLikeFile(const QFileInfo &info) const;
    QString nodeLocationDescription(const DataNode &node) const;
    void populateDuplicateAndCandidateFlags(AnalysisResult *result,
                                           const QSet<QString> &whitelistIds) const;
    QString relativePath(const QString &rootFolder, const QString &absolutePath) const;
};
