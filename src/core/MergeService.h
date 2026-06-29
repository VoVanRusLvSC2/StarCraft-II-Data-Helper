#pragma once

#include "core/AnalysisModels.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <functional>

struct MergeRequest
{
    int keepNodeIndex = -1;
    QVector<int> removeNodeIndices;
};

struct MergePreview
{
    bool valid = false;
    QString keptId;
    QStringList removedIds;
    QStringList filesChanged;
    int fieldsChanged = 0;
    int referencesRedirected = 0;
    int nodesDeleted = 0;
    QStringList changes;
    QStringList warnings;
    QString riskLevel;
    QString reportText;
};

struct MergeApplyResult
{
    bool success = false;
    QString backupFolder;
    QStringList changedFiles;
    int referencesRedirected = 0;
    int nodesDeleted = 0;
    int skippedMerges = 0;
    QStringList warnings;
    QString error;
};

class MergeService
{
public:
    MergePreview preview(const AnalysisResult &analysis, const MergeRequest &request) const;
    MergeApplyResult apply(const AnalysisResult &analysis,
                           const MergeRequest &request,
                           const QString &rootFolder,
                           const QSet<QString> &whitelistIds) const;
    MergeApplyResult applyBatch(const AnalysisResult &analysis,
                                const QVector<MergeRequest> &requests,
                                const QString &rootFolder,
                                const QSet<QString> &whitelistIds,
                                const std::function<void(int, int, const QString &)> &progress = {}) const;

    static int replaceIdTokens(QString *value, const QString &oldId, const QString &newId);
    static int countIdTokens(const QString &value, const QString &id);

    // Deterministic transaction-failure hook used by rollback tests.
    void setFailureInjectionStep(const QString &step) { m_failureInjectionStep = step; }

private:
    QString m_failureInjectionStep;
};
