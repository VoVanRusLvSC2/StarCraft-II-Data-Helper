#pragma once

#include "core/AnalysisModels.h"

#include <QVector>

class SemanticDuplicateAnalyzer
{
public:
    void appendCandidates(const AnalysisResult &analysis, QVector<DeepCleanupCandidate> *candidates) const;
};
