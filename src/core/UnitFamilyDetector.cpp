#include "core/UnitFamilyDetector.h"

#include <QHash>
#include <QQueue>
#include <QSet>

namespace {

bool isCatalogType(const QString &type)
{
    static const QStringList types = {QStringLiteral("CActor"), QStringLiteral("CModel"), QStringLiteral("CSound"),
        QStringLiteral("CButton"), QStringLiteral("CWeapon"), QStringLiteral("CAbil"), QStringLiteral("CEffect"),
        QStringLiteral("CBehavior"), QStringLiteral("CValidator"), QStringLiteral("CRequirement"), QStringLiteral("CUpgrade")};
    for (const QString &prefix : types) if (type.startsWith(prefix, Qt::CaseInsensitive)) return true;
    return false;
}

UnitFamilyRole roleFromNode(const DataNode &node, const QString &root, QString *confidence, QString *reason)
{
    const QString type = node.elementName.toLower();
    const QString id = node.id.toLower();
    const QString rootLower = root.toLower();
    const auto contains = [&id](const char *text) { return id.contains(QString::fromLatin1(text), Qt::CaseInsensitive); };
    if (type == QStringLiteral("cunit")) return UnitFamilyRole::Unit;
    if (type.startsWith(QStringLiteral("cactorunit"))) {
        if (node.attributes.value(QStringLiteral("unitName")) == root || node.referencedIds.contains(root)) {
            *confidence = QStringLiteral("High"); *reason = QStringLiteral("Actor unitName/direct token references the root unit");
        }
        return UnitFamilyRole::Actor;
    }
    if (type.startsWith(QStringLiteral("cbutton"))) return UnitFamilyRole::Button;
    if (type.startsWith(QStringLiteral("cmodel"))) {
        if (contains("deathdisintegrate")) return UnitFamilyRole::DeathDisintegrateModel;
        if (contains("deathfire")) return UnitFamilyRole::DeathFireModel;
        if (contains("deathblast")) return UnitFamilyRole::DeathBlastModel;
        if (contains("portrait")) return UnitFamilyRole::PortraitModel;
        if (contains("death")) return UnitFamilyRole::DeathModel;
        return UnitFamilyRole::Model;
    }
    if (type.startsWith(QStringLiteral("csound"))) {
        if (contains("deathvoice")) return UnitFamilyRole::DeathVoice;
        if (contains("attack")) return UnitFamilyRole::Attack;
        if (contains("help")) return UnitFamilyRole::Help;
        if (contains("pissed")) return UnitFamilyRole::Pissed;
        if (contains("yes")) return UnitFamilyRole::Yes;
        if (contains("what")) return UnitFamilyRole::What;
        if (contains("ready")) return UnitFamilyRole::Ready;
        if (contains("death")) return UnitFamilyRole::Death;
        return UnitFamilyRole::ManualReview;
    }
    if (type.startsWith(QStringLiteral("cweapon"))) return UnitFamilyRole::Weapon;
    if (type.startsWith(QStringLiteral("cabil"))) return UnitFamilyRole::Ability;
    if (type.startsWith(QStringLiteral("ceffect"))) return UnitFamilyRole::Effect;
    if (type.startsWith(QStringLiteral("cbehavior"))) return UnitFamilyRole::Behavior;
    if (type.startsWith(QStringLiteral("cvalidator"))) return UnitFamilyRole::Validator;
    if (type.startsWith(QStringLiteral("crequirement"))) return UnitFamilyRole::Requirement;
    if (type.startsWith(QStringLiteral("cupgrade"))) return UnitFamilyRole::Upgrade;
    if (id.startsWith(rootLower) || id.endsWith(rootLower)) return UnitFamilyRole::Other;
    return UnitFamilyRole::ManualReview;
}

} // namespace

QString unitFamilyRoleName(UnitFamilyRole role)
{
    switch (role) {
    case UnitFamilyRole::Unit: return QStringLiteral("Unit");
    case UnitFamilyRole::Actor: return QStringLiteral("Actor");
    case UnitFamilyRole::Button: return QStringLiteral("Button");
    case UnitFamilyRole::Model: return QStringLiteral("Model");
    case UnitFamilyRole::DeathModel: return QStringLiteral("DeathModel");
    case UnitFamilyRole::DeathFireModel: return QStringLiteral("DeathFireModel");
    case UnitFamilyRole::DeathDisintegrateModel: return QStringLiteral("DeathDisintegrateModel");
    case UnitFamilyRole::DeathBlastModel: return QStringLiteral("DeathBlastModel");
    case UnitFamilyRole::PortraitModel: return QStringLiteral("PortraitModel");
    case UnitFamilyRole::DeathVoice: return QStringLiteral("DeathVoice");
    case UnitFamilyRole::Death: return QStringLiteral("Death");
    case UnitFamilyRole::Attack: return QStringLiteral("Attack");
    case UnitFamilyRole::Help: return QStringLiteral("Help");
    case UnitFamilyRole::Pissed: return QStringLiteral("Pissed");
    case UnitFamilyRole::Yes: return QStringLiteral("Yes");
    case UnitFamilyRole::What: return QStringLiteral("What");
    case UnitFamilyRole::Ready: return QStringLiteral("Ready");
    case UnitFamilyRole::Weapon: return QStringLiteral("Weapon");
    case UnitFamilyRole::Ability: return QStringLiteral("Ability");
    case UnitFamilyRole::Effect: return QStringLiteral("Effect");
    case UnitFamilyRole::Behavior: return QStringLiteral("Behavior");
    case UnitFamilyRole::Validator: return QStringLiteral("Validator");
    case UnitFamilyRole::Requirement: return QStringLiteral("Requirement");
    case UnitFamilyRole::Upgrade: return QStringLiteral("Upgrade");
    case UnitFamilyRole::Other: return QStringLiteral("Other");
    case UnitFamilyRole::ManualReview: return QStringLiteral("Manual Review");
    }
    return QStringLiteral("Manual Review");
}

QVector<UnitFamily> UnitFamilyDetector::detect(const AnalysisResult &analysis) const
{
    QHash<QString, int> byId;
    for (int i = 0; i < analysis.nodes.size(); ++i) if (!analysis.nodes[i].id.isEmpty()) byId.insert(analysis.nodes[i].id, i);
    QVector<UnitFamily> families;
    for (int rootIndex = 0; rootIndex < analysis.nodes.size(); ++rootIndex) {
        const DataNode &root = analysis.nodes[rootIndex];
        if (root.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) != 0) continue;
        UnitFamily family;
        family.rootNodeIndex = rootIndex;
        family.rootId = root.id;
        QSet<int> included{rootIndex};
        QHash<int, QString> evidence;
        evidence[rootIndex] = QStringLiteral("Root CUnit");

        // Proven inbound actor relation and same-root names establish seeds.
        for (int i = 0; i < analysis.nodes.size(); ++i) {
            if (i == rootIndex) continue;
            const DataNode &node = analysis.nodes[i];
            const bool actorLink = node.elementName.startsWith(QStringLiteral("CActorUnit"), Qt::CaseInsensitive)
                && (node.attributes.value(QStringLiteral("unitName")) == root.id || node.referencedIds.contains(root.id));
            const bool sameName = isCatalogType(node.elementName)
                && (node.id.startsWith(root.id, Qt::CaseInsensitive) || node.id.endsWith(root.id, Qt::CaseInsensitive));
            if (actorLink || sameName) {
                included.insert(i);
                evidence[i] = actorLink ? QStringLiteral("Proven actor field references root unit") : QStringLiteral("Name similarity only (same-root naming)");
            }
        }

        // Follow outgoing graph edges from root and established family objects.
        QQueue<QPair<int, int>> queue;
        for (int seed : included) queue.enqueue({seed, 0});
        while (!queue.isEmpty()) {
            const auto [index, depth] = queue.dequeue();
            if (depth >= 4) continue;
            for (const QString &reference : analysis.nodes[index].referencedIds) {
                const int target = byId.value(reference, -1);
                if (target < 0 || included.contains(target) || !isCatalogType(analysis.nodes[target].elementName)) continue;
                included.insert(target);
                const DataNode &source = analysis.nodes[index];
                const DataNode &targetNode = analysis.nodes[target];
                const bool provenFieldRole = (source.elementName.startsWith(QStringLiteral("CActor"), Qt::CaseInsensitive)
                                               && (targetNode.elementName.startsWith(QStringLiteral("CModel"), Qt::CaseInsensitive)
                                                   || targetNode.elementName.startsWith(QStringLiteral("CSound"), Qt::CaseInsensitive)))
                    || (source.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0
                        && (targetNode.elementName.startsWith(QStringLiteral("CButton"), Qt::CaseInsensitive)
                            || targetNode.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive)
                            || targetNode.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive)));
                evidence[target] = provenFieldRole
                    ? QStringLiteral("Proven SC2 field/reference from %1").arg(source.id)
                    : QStringLiteral("Outgoing reference graph relation from %1").arg(source.id);
                queue.enqueue({target, depth + 1});
            }
        }

        QList<int> ordered = included.values();
        std::sort(ordered.begin(), ordered.end());
        for (int index : ordered) {
            QString confidence = index == rootIndex || evidence[index].startsWith(QStringLiteral("Proven"))
                ? QStringLiteral("High")
                : evidence[index].startsWith(QStringLiteral("Outgoing")) ? QStringLiteral("Medium") : QStringLiteral("Low");
            QString reason = evidence.value(index);
            UnitFamilyRole role = roleFromNode(analysis.nodes[index], root.id, &confidence, &reason);
            if (role == UnitFamilyRole::ManualReview) {
                confidence = QStringLiteral("Low");
                reason += QStringLiteral("; role is ambiguous");
            }
            family.objects.append({index, role, confidence, reason});
        }
        families.append(family);
    }
    return families;
}
