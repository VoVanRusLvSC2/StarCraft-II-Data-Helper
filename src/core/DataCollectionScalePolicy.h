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

inline DataCollectionScaleAssessment assessDataCollectionScale(const QVector<UnitFamily> &families,
                                                                bool aggressiveClosedProject = false)
{
    const int maximumAutomaticFamilies = aggressiveClosedProject ? 10000 : 1024;
    const qsizetype maximumAutomaticMemberships = aggressiveClosedProject ? 100000 : 20000;
    const int maximumAutomaticFamilySize = aggressiveClosedProject ? 50000 : 2048;

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

    result.automaticBatchAllowed = result.familyCount <= maximumAutomaticFamilies
        && result.totalMemberships <= maximumAutomaticMemberships
        && result.largestFamily <= maximumAutomaticFamilySize;
    if (!result.automaticBatchAllowed)
    {
        result.reason = QStringLiteral("Large Data Collection plan requires explicit review%5: %1 families, %2 memberships; largest family %3 has %4 objects.")
                            .arg(result.familyCount)
                            .arg(result.totalMemberships)
                            .arg(result.largestFamilyRoot)
                            .arg(result.largestFamily)
                            .arg(aggressiveClosedProject ? QStringLiteral(" even in Closed Project mode") : QString());
    }
    return result;
}
