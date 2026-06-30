#pragma once

#include "core/StandardNamePlanner.h"

#include <QHash>
#include <QSet>
#include <functional>

struct RenamePreviewReport
{
    bool valid = false;
    RenamePlan plan;
    QStringList filesChanged;
    QStringList referenceChanges;
    QStringList warnings;
    QStringList conflicts;
    int identitiesRenamed = 0;
    int referencesUpdated = 0;
    QString reportText;
};

struct RenameApplyResult
{
    bool success = false;
    QString backupFolder;
    QStringList changedFiles;
    QHash<QString, QString> appliedRenames;
    int identitiesRenamed = 0;
    int referencesUpdated = 0;
    QStringList warnings;
    QString error;
    QString finalReport;
};

class ReferenceRenamer
{
public:
    using ProgressCallback = std::function<void(const QString &stage, int index, int total, const QString &file)>;

    RenamePreviewReport preview(const AnalysisResult &analysis, const RenamePlan &plan) const;
    RenameApplyResult apply(const AnalysisResult &analysis, const RenamePlan &plan,
                            const QString &rootFolder, const QSet<QString> &whitelistIds,
                            const ProgressCallback &progress = {}) const;
    void setFailureInjectionStep(const QString &step) { m_failureInjectionStep = step; }

private:
    QString m_failureInjectionStep;
};
