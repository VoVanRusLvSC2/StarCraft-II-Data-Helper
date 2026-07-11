#pragma once

#include "core/AnalysisModels.h"
#include "core/DeepCleanupService.h"

#include <QFileInfo>

inline bool sourceMayHaveExternalConsumers(const QString &sourcePath)
{
    const QString suffix = QFileInfo(sourcePath).suffix();
    return suffix.compare(QStringLiteral("SC2Mod"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("SC2Campaign"), Qt::CaseInsensitive) == 0;
}

inline void applyExternalConsumerSafety(AnalysisResult *analysis)
{
    if (!analysis || !analysis->externalConsumersUnknown)
        return;

    analysis->possibleUnusedNodeIndices.clear();
    for (UnusedCandidateInfo &candidate : analysis->unusedCandidates)
    {
        if (candidate.state != CandidateState::Safe)
            continue;
        candidate.state = CandidateState::Blocked;
        candidate.usageState = UsageState::Blocked;
        candidate.protectedObject = true;
        candidate.reason = QStringLiteral("The object can be referenced by external maps/mods that were not analyzed together.");
        candidate.riskLevel = QStringLiteral("high");
        if (candidate.nodeIndex >= 0 && candidate.nodeIndex < analysis->nodes.size())
            analysis->nodes[candidate.nodeIndex].candidateUnused = false;
    }

    DeepCleanupService().populateCandidates(analysis);
}
