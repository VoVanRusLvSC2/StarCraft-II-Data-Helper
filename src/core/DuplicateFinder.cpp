#include "core/DuplicateFinder.h"

QVector<DuplicateGroup> DuplicateFinder::findDuplicateIds(const QVector<DataNode> &nodes) const
{
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id.isEmpty()) {
            continue;
        }
        groups[nodes[i].id].append(i);
    }

    QVector<DuplicateGroup> duplicates;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        if (it.value().size() > 1) {
            duplicates.push_back({it.key(), it.value()});
        }
    }
    return duplicates;
}

QVector<DuplicateGroup> DuplicateFinder::findDuplicateContentHashes(const QVector<DataNode> &nodes) const
{
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes[i].contentHash.isEmpty()) {
            continue;
        }
        const QString bodyKey = nodes[i].elementName + QChar(0x1f) + nodes[i].contentHash;
        groups[bodyKey].append(i);
    }

    QVector<DuplicateGroup> duplicates;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        if (it.value().size() > 1) {
            duplicates.push_back({it.key(), it.value()});
        }
    }
    return duplicates;
}
