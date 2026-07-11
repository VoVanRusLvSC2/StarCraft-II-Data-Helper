#pragma once

#include "core/UnitFamilyDetector.h"

#include <QString>

struct DataCollectionScaleAssessment
{
    int familyCount = 0;
    qsizetype totalMemberships = 0;
    int largestFamily = 0;
    QString largestFamilyRoot;
    bool automaticBatchAllowed = true;
    QString reason;
};

inline DataCollectionScaleAssessment assessDataCollectionScale(const QVector<UnitFamily> &families)
{
    constexpr int kMaximumAutomaticFamilies = 1024;
    constexpr qsizetype kMaximumAutomaticMemberships = 20000;
    constexpr int kMaximumAutomaticFamilySize = 2048;

    DataCollectionScaleAssessment result;
    result.familyCount = families.size();
    for (const UnitFamily &family : families)
    {
        result.totalMemberships += family.objects.size();
        if (family.objects.size() > result.largestFamily)
        {
            result.largestFamily = family.objects.size();
            result.largestFamilyRoot = family.rootId;
        }
    }

    result.automaticBatchAllowed = result.familyCount <= kMaximumAutomaticFamilies
        && result.totalMemberships <= kMaximumAutomaticMemberships
        && result.largestFamily <= kMaximumAutomaticFamilySize;
    if (!result.automaticBatchAllowed)
    {
        result.reason = QStringLiteral("Large Data Collection plan requires explicit review: %1 families, %2 memberships; largest family %3 has %4 objects.")
                            .arg(result.familyCount)
                            .arg(result.totalMemberships)
                            .arg(result.largestFamilyRoot)
                            .arg(result.largestFamily);
    }
    return result;
}
