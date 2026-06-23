#include "core/ReferenceGraph.h"

#include <QSet>

#include <algorithm>

void ReferenceGraph::build(const QVector<DataNode> &nodes)
{
    m_inbound.clear();
    m_outbound.clear();

    QHash<QString, QSet<QString>> inboundSets;
    QHash<QString, QSet<QString>> outboundSets;
    for (const DataNode &node : nodes) {
        for (const QString &reference : node.referencedIds) {
            if (reference.isEmpty() || node.id.isEmpty()) {
                continue;
            }
            outboundSets[node.id].insert(reference);
            inboundSets[reference].insert(node.id);
        }
    }

    for (auto it = outboundSets.cbegin(); it != outboundSets.cend(); ++it) {
        QStringList values = it.value().values();
        std::sort(values.begin(), values.end(), [](const QString &left, const QString &right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });
        m_outbound.insert(it.key(), values);
    }

    for (auto it = inboundSets.cbegin(); it != inboundSets.cend(); ++it) {
        QStringList values = it.value().values();
        std::sort(values.begin(), values.end(), [](const QString &left, const QString &right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });
        m_inbound.insert(it.key(), values);
    }
}

QStringList ReferenceGraph::inboundReferencesFor(const QString &id) const
{
    return m_inbound.value(id);
}

QStringList ReferenceGraph::outboundReferencesFor(const QString &id) const
{
    return m_outbound.value(id);
}
