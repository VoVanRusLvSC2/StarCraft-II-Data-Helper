#pragma once

#include "core/DataNode.h"
#include "core/ReferenceGraph.h"

#include <QSet>
#include <QVector>

class UnusedFinder
{
public:
    QVector<int> findCandidateUnused(const QVector<DataNode> &nodes,
                                     const ReferenceGraph &graph,
                                     const QSet<QString> &whitelist) const;
};
