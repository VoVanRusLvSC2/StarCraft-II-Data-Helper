#pragma once

#include "core/AnalysisModels.h"

#include <QVector>

class AssetOptimizationAnalyzer
{
public:
    void appendCandidates(const AnalysisResult &analysis, QVector<DeepCleanupCandidate> *candidates) const;
};
