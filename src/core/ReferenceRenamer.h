#pragma once

#include "core/StandardNamePlanner.h"

#include <QSet>

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
    int identitiesRenamed = 0;
    int referencesUpdated = 0;
    QString error;
    QString finalReport;
};

class ReferenceRenamer
{
public:
    RenamePreviewReport preview(const AnalysisResult &analysis, const RenamePlan &plan) const;
    RenameApplyResult apply(const AnalysisResult &analysis, const RenamePlan &plan,
                            const QString &rootFolder, const QSet<QString> &whitelistIds) const;
    void setFailureInjectionStep(const QString &step) { m_failureInjectionStep = step; }

private:
    QString m_failureInjectionStep;
};
