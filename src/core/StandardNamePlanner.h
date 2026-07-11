#pragma once

#include "core/UnitFamilyDetector.h"

#include <QHash>
#include <QSet>

struct StandardNamePlanningIndex
{
    QSet<QString> existingIdentities;
    QHash<QString, QSet<QString>> scopesById;
};

struct RenamePlanItem
{
    int nodeIndex = -1;
    UnitFamilyRole role = UnitFamilyRole::ManualReview;
    QString oldId;
    QString newId;
    QString confidence;
    QString riskLevel;
    QString reason;
    bool selected = true;
    bool blocked = false;
    QString conflict;
};

struct RenamePlan
{
    UnitFamily family;
    QString targetRootId;
    QVector<RenamePlanItem> items;
    QVector<UnitFamilyObject> manualReview;
    QStringList conflicts;
    QStringList warnings;
    bool valid = false;
};

class StandardNamePlanner
{
public:
    StandardNamePlanningIndex buildIndex(const AnalysisResult &analysis) const;
    RenamePlan plan(const AnalysisResult &analysis, const UnitFamily &family,
                    const QString &targetRootId, const QSet<int> &includedNodeIndices = {},
                    const StandardNamePlanningIndex *preparedIndex = nullptr) const;
};
