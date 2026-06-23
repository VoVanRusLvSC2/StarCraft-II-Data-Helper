#include "core/UnusedFinder.h"

QVector<int> UnusedFinder::findCandidateUnused(const QVector<DataNode> &nodes,
                                               const ReferenceGraph &graph,
                                               const QSet<QString> &whitelist) const
{
    QVector<int> candidates;
    for (int i = 0; i < nodes.size(); ++i) {
        const DataNode &node = nodes[i];
        if (node.id.isEmpty() || whitelist.contains(node.id)) {
            continue;
        }

        if (graph.inboundReferencesFor(node.id).isEmpty()) {
            candidates.append(i);
        }
    }
    return candidates;
}
