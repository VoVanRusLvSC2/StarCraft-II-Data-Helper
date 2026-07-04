#include "core/AnalysisIndex.h"

AnalysisIndex::AnalysisIndex(const AnalysisResult &analysis)
{
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        const DataNode &node = analysis.nodes.at(i);
        if (node.id.isEmpty())
            continue;
        m_knownIds.insert(node.id);
        m_nodesByTypeAndId.insert(typeIdKey(node.elementName, node.id), &node);
        m_nodeIndicesById[node.id].append(i);
    }
}

QString AnalysisIndex::typeIdKey(const QString &elementName, const QString &id)
{
    return elementName.toLower() + QChar(0x1f) + id.toLower();
}
