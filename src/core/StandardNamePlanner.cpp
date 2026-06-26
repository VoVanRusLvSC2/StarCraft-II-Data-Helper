#include "core/StandardNamePlanner.h"

#include <QHash>
#include <QRegularExpression>

RenamePlan StandardNamePlanner::plan(const AnalysisResult &analysis, const UnitFamily &family,
                                     const QString &targetRootId, const QSet<int> &includedNodeIndices) const
{
    RenamePlan result;
    result.family = family;
    result.targetRootId = targetRootId.trimmed();
    const QRegularExpression validId(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    if (!validId.match(result.targetRootId).hasMatch() || result.targetRootId.contains(QLatin1Char('@'))) {
        result.conflicts << QStringLiteral("Target root ID must be a real XML ID without @.");
    }
    QSet<QString> existingIds;
    for (const DataNode &node : analysis.nodes) existingIds.insert(node.id);
    QHash<UnitFamilyRole, int> roleCounts;
    for (const UnitFamilyObject &object : family.objects) ++roleCounts[object.role];
    QHash<UnitFamilyRole, int> roleOrdinals;
    QHash<QString, int> proposedCounts;

    for (const UnitFamilyObject &object : family.objects) {
        const DataNode &node = analysis.nodes[object.nodeIndex];
        if (!includedNodeIndices.isEmpty() && !includedNodeIndices.contains(object.nodeIndex)) continue;
        if (object.role == UnitFamilyRole::ManualReview || object.role == UnitFamilyRole::Other) {
            result.manualReview << object;
            continue;
        }
        const QString role = unitFamilyRoleName(object.role);
        QString expected;
        if (object.nodeIndex == family.rootNodeIndex) {
            expected = result.targetRootId;
        } else {
            const int ordinal = ++roleOrdinals[object.role];
            QString suffix;
            if (node.id.startsWith(family.rootId, Qt::CaseInsensitive)) {
                suffix = node.id.mid(family.rootId.size());
                while (!suffix.isEmpty() && QStringLiteral("@_-").contains(suffix.front())) suffix.remove(0, 1);
                suffix.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")));
            }
            if (suffix.isEmpty() || suffix.compare(family.rootId, Qt::CaseInsensitive) == 0)
                suffix = role + (roleCounts.value(object.role) > 1 && ordinal > 1 ? QString::number(ordinal) : QString());
            expected = result.targetRootId + QLatin1Char('@') + suffix;
        }
        if (node.id == expected) continue;
        RenamePlanItem item;
        item.nodeIndex = object.nodeIndex;
        item.role = object.role;
        item.oldId = node.id;
        item.newId = expected;
        item.confidence = object.confidence;
        item.reason = object.reason;
        item.riskLevel = object.confidence == QStringLiteral("High") ? QStringLiteral("Low") : QStringLiteral("Medium");
        result.items << item;
        ++proposedCounts[expected];
    }

    QSet<QString> renamedOldIds;
    for (const RenamePlanItem &item : result.items) renamedOldIds.insert(item.oldId);
    for (RenamePlanItem &item : result.items) {
        if (proposedCounts.value(item.newId) > 1) {
            item.blocked = true; item.conflict = QStringLiteral("Duplicate proposed ID");
        } else if (existingIds.contains(item.newId) && !renamedOldIds.contains(item.newId)) {
            item.blocked = true; item.conflict = QStringLiteral("Target ID already exists");
        }
        if (item.blocked) result.conflicts << QStringLiteral("%1 -> %2: %3").arg(item.oldId, item.newId, item.conflict);
    }
    if (!result.manualReview.isEmpty()) result.warnings << QStringLiteral("Unsupported ambiguous objects require manual review and are excluded from renaming.");
    result.valid = result.conflicts.isEmpty() && !result.items.isEmpty();
    return result;
}
