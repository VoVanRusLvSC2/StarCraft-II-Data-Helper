#include "core/StandardNamePlanner.h"

#include "core/CatalogProtection.h"

#include <QHash>
#include <QRegularExpression>

namespace {

QString actorUnitName(const DataNode &node)
{
    if (!node.elementName.startsWith(QStringLiteral("CActorUnit"), Qt::CaseInsensitive))
        return {};
    return node.attributes.value(QStringLiteral("unitName")).trimmed();
}

} // namespace

RenamePlan StandardNamePlanner::plan(const AnalysisResult &analysis, const UnitFamily &family,
                                     const QString &targetRootId, const QSet<int> &includedNodeIndices) const
{
    RenamePlan result;
    result.family = family;
    result.targetRootId = targetRootId.trimmed();
    if (result.targetRootId.endsWith(QStringLiteral("@Unit"), Qt::CaseInsensitive))
        result.targetRootId.chop(5);
    const QRegularExpression validId(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    if (!validId.match(result.targetRootId).hasMatch() || result.targetRootId.contains(QLatin1Char('@'))) {
        result.conflicts << QStringLiteral("Target root ID must be a real XML ID without @.");
    }
    if (sc2dh::isReservedCatalogToken(result.targetRootId) || sc2dh::isKnownBlizzardCatalogId(result.targetRootId)) {
        result.conflicts << QStringLiteral("Target root ID is a reserved SC2/Blizzard catalog token and cannot be renamed automatically.");
    }
    const auto identityKey = [](const QString &elementName, const QString &id) {
        return sc2dh::catalogIdentityKey(elementName, id);
    };

    QSet<QString> existingIdentities;
    QHash<QString, QSet<QString>> scopesById;
    for (const DataNode &node : analysis.nodes)
        if (!node.id.isEmpty()) {
            existingIdentities.insert(identityKey(node.elementName, node.id));
            const QString scope = sc2dh::catalogIdentityScope(node.elementName);
            if (!scope.isEmpty())
                scopesById[node.id.toCaseFolded()].insert(scope);
        }

    QSet<QString> familyUnitIds;
    familyUnitIds.insert(family.rootId.toCaseFolded());
    for (const UnitFamilyObject &object : family.objects) {
        if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size())
            continue;
        if (object.role == UnitFamilyRole::Unit)
            familyUnitIds.insert(analysis.nodes[object.nodeIndex].id.toCaseFolded());
    }

    QHash<UnitFamilyRole, int> roleCounts;
    for (const UnitFamilyObject &object : family.objects) ++roleCounts[object.role];
    QHash<UnitFamilyRole, int> roleOrdinals;
    QHash<QString, int> actorUnitOrdinals;
    QHash<QString, int> proposedCounts;

    for (const UnitFamilyObject &object : family.objects) {
        const DataNode &node = analysis.nodes[object.nodeIndex];
        if (!includedNodeIndices.isEmpty() && !includedNodeIndices.contains(object.nodeIndex)) continue;
        if (sc2dh::isProtectedCatalogNode(node)) {
            UnitFamilyObject manual = object;
            manual.confidence = QStringLiteral("Low");
            manual.reason += QStringLiteral("; protected standard/dependency catalog object");
            result.manualReview << manual;
            continue;
        }
        if (sc2dh::isReservedCatalogToken(node.id)) {
            UnitFamilyObject manual = object;
            manual.confidence = QStringLiteral("Low");
            manual.reason += QStringLiteral("; reserved SC2 engine token, not a safe object identity for automatic rename");
            result.manualReview << manual;
            continue;
        }
        if (!sc2dh::isSafeAutomaticObjectId(node.id)) {
            UnitFamilyObject manual = object;
            manual.confidence = QStringLiteral("Low");
            manual.reason += QStringLiteral("; object ID is numeric or not safe for automatic reference rewriting");
            result.manualReview << manual;
            continue;
        }
        if (object.role == UnitFamilyRole::ManualReview || object.role == UnitFamilyRole::Other) {
            result.manualReview << object;
            continue;
        }
        if (node.elementName.startsWith(QStringLiteral("CModel"), Qt::CaseInsensitive)
            && scopesById.value(node.id.toCaseFolded()).contains(QStringLiteral("cactor"))) {
            UnitFamilyObject manual = object;
            manual.confidence = QStringLiteral("Low");
            manual.reason += QStringLiteral("; model shares its ID with an actor/doodad, which SC2 placement can resolve implicitly");
            result.manualReview << manual;
            continue;
        }
        const QString role = unitFamilyRoleName(object.role);
        QString expected;
        if (object.nodeIndex == family.rootNodeIndex) {
            expected = result.targetRootId;
        } else if (object.role == UnitFamilyRole::Actor
                   && node.elementName.startsWith(QStringLiteral("CActorUnit"), Qt::CaseInsensitive)) {
            const QString unitName = actorUnitName(node);
            if (unitName.isEmpty() || !familyUnitIds.contains(unitName.toCaseFolded())) {
                UnitFamilyObject manual = object;
                manual.confidence = QStringLiteral("Low");
                manual.reason += unitName.isEmpty()
                    ? QStringLiteral("; actor unitName is empty, so automatic rename could break SC2 actor scope")
                    : QStringLiteral("; actor unitName points outside this family, so automatic rename could create duplicate unit-scope actors");
                result.manualReview << manual;
                continue;
            }

            const QString targetUnitId = unitName.compare(family.rootId, Qt::CaseInsensitive) == 0
                ? result.targetRootId
                : unitName;
            const int ordinal = ++actorUnitOrdinals[targetUnitId.toCaseFolded()];
            expected = ordinal == 1 ? targetUnitId
                                    : targetUnitId + QStringLiteral("@Actor") + QString::number(ordinal);
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
        ++proposedCounts[identityKey(node.elementName, expected)];
    }

    QSet<QString> renamedOldIdentities;
    for (const RenamePlanItem &item : result.items) {
        const DataNode &node = analysis.nodes[item.nodeIndex];
        renamedOldIdentities.insert(identityKey(node.elementName, item.oldId));
    }
    for (RenamePlanItem &item : result.items) {
        const DataNode &node = analysis.nodes[item.nodeIndex];
        const QString newIdentity = identityKey(node.elementName, item.newId);
        if (proposedCounts.value(newIdentity) > 1) {
            item.blocked = true; item.conflict = QStringLiteral("Duplicate proposed ID");
        } else if (existingIdentities.contains(newIdentity) && !renamedOldIdentities.contains(newIdentity)) {
            item.blocked = true; item.conflict = QStringLiteral("Target ID already exists");
        }
        if (item.blocked) result.conflicts << QStringLiteral("%1 -> %2: %3").arg(item.oldId, item.newId, item.conflict);
    }
    if (!result.manualReview.isEmpty()) result.warnings << QStringLiteral("Unsupported ambiguous objects require manual review and are excluded from renaming.");
    result.valid = result.conflicts.isEmpty() && !result.items.isEmpty();
    return result;
}
