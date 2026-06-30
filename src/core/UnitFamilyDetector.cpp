#include "core/UnitFamilyDetector.h"

#include "core/DataCollectionAliasMapper.h"
#include "core/CatalogProtection.h"

#include <QHash>
#include <QQueue>
#include <QSet>
#include <QTextStream>

#include <pugixml.hpp>

namespace {

void traceCollectionDetect(const QString &stage, const QString &detail = {})
{
    if (!qEnvironmentVariableIsSet("SC2DH_TRACE_COLLECTION_DETECT"))
        return;
    QTextStream err(stderr);
    err << "dc_detect stage=" << stage;
    if (!detail.isEmpty())
        err << ' ' << detail;
    err << '\n';
    err.flush();
}

bool isCatalogType(const QString &type)
{
    if (sc2dh::isEditorRuntimeCatalogType(type))
        return false;
    static const QStringList types = {QStringLiteral("CActor"), QStringLiteral("CModel"), QStringLiteral("CSound"),
        QStringLiteral("CButton"), QStringLiteral("CWeapon"), QStringLiteral("CAbil"), QStringLiteral("CEffect"),
        QStringLiteral("CBehavior"), QStringLiteral("CValidator"), QStringLiteral("CRequirement"), QStringLiteral("CUpgrade"),
        QStringLiteral("CMover"), QStringLiteral("CTurret"), QStringLiteral("CFootprint"), QStringLiteral("CSiteOp"),
        QStringLiteral("CBeam")};
    for (const QString &prefix : types) if (type.startsWith(prefix, Qt::CaseInsensitive)) return true;
    return false;
}

QString standardDataCollectionParent(DataCollectionEntityType entityType, const QHash<int, QStringList> &paths,
                                     const QVector<DataNode> &nodes)
{
    if (entityType == DataCollectionEntityType::Unit)
        return QStringLiteral("UnitGround");
    if (entityType == DataCollectionEntityType::Weapon)
        return QStringLiteral("Weapon_Instant");
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        const int index = it.key();
        if (index >= 0 && index < nodes.size()
            && nodes[index].elementName.startsWith(QStringLiteral("CMover"), Qt::CaseInsensitive))
            return QStringLiteral("AbilityMisssile");
    }
    return QStringLiteral("AbilityBase");
}

QString standardDataCollectionParent(DataCollectionEntityType entityType, const QSet<int> &indices,
                                     const QVector<DataNode> &nodes)
{
    if (entityType == DataCollectionEntityType::Unit)
        return QStringLiteral("UnitGround");
    if (entityType == DataCollectionEntityType::Weapon)
        return QStringLiteral("Weapon_Instant");
    for (int index : indices) {
        if (index >= 0 && index < nodes.size()
            && nodes[index].elementName.startsWith(QStringLiteral("CMover"), Qt::CaseInsensitive))
            return QStringLiteral("AbilityMisssile");
    }
    return QStringLiteral("AbilityBase");
}

UnitFamilyRole roleFromNode(const DataNode &node, const QString &root, QString *confidence, QString *reason)
{
    const QString type = node.elementName.toLower();
    const QString id = node.id.toLower();
    const QString rootLower = root.toLower();
    const auto contains = [&id](const char *text) { return id.contains(QString::fromLatin1(text), Qt::CaseInsensitive); };
    if (sc2dh::isEditorRuntimeCatalogType(node.elementName))
        return UnitFamilyRole::ManualReview;
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
        return UnitFamilyRole::Sound;
    }
    if (type.startsWith(QStringLiteral("cweapon"))) return UnitFamilyRole::Weapon;
    if (type.startsWith(QStringLiteral("cabil"))) return UnitFamilyRole::Ability;
    if (type.startsWith(QStringLiteral("ceffect"))) return UnitFamilyRole::Effect;
    if (type.startsWith(QStringLiteral("cbehavior"))) return UnitFamilyRole::Behavior;
    if (type.startsWith(QStringLiteral("cvalidator"))) return UnitFamilyRole::Validator;
    if (type.startsWith(QStringLiteral("crequirementnode"))) return UnitFamilyRole::RequirementNode;
    if (type.startsWith(QStringLiteral("crequirement"))) return UnitFamilyRole::Requirement;
    if (type.startsWith(QStringLiteral("cupgrade"))) return UnitFamilyRole::Upgrade;
    if (type.startsWith(QStringLiteral("cmover"))) return UnitFamilyRole::Mover;
    if (type.startsWith(QStringLiteral("cturret"))) return UnitFamilyRole::Turret;
    if (type.startsWith(QStringLiteral("cfootprint"))) return UnitFamilyRole::Footprint;
    if (type.startsWith(QStringLiteral("csiteop"))) return UnitFamilyRole::SiteOp;
    if (type.startsWith(QStringLiteral("cbeam"))) return UnitFamilyRole::Beam;
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
    case UnitFamilyRole::Sound: return QStringLiteral("Sound");
    case UnitFamilyRole::Mover: return QStringLiteral("Mover");
    case UnitFamilyRole::Turret: return QStringLiteral("Turret");
    case UnitFamilyRole::Footprint: return QStringLiteral("Footprint");
    case UnitFamilyRole::SiteOp: return QStringLiteral("SiteOp");
    case UnitFamilyRole::Beam: return QStringLiteral("Beam");
    case UnitFamilyRole::Texture: return QStringLiteral("Texture");
    case UnitFamilyRole::RequirementNode: return QStringLiteral("RequirementNode");
    case UnitFamilyRole::Other: return QStringLiteral("Other");
    case UnitFamilyRole::ManualReview: return QStringLiteral("Manual Review");
    }
    return QStringLiteral("Manual Review");
}

QString dataCollectionEntityTypeName(DataCollectionEntityType type)
{
    switch (type) {
    case DataCollectionEntityType::Unit: return QStringLiteral("Unit");
    case DataCollectionEntityType::Ability: return QStringLiteral("Ability");
    case DataCollectionEntityType::Weapon: return QStringLiteral("Weapon");
    }
    return QStringLiteral("Unit");
}

QVector<UnitFamily> UnitFamilyDetector::detect(const AnalysisResult &analysis) const
{
    QHash<QString, int> byId;
    QVector<int> rootIndices;
    QSet<QString> rootIds;
    QSet<int> rootIdLengths;
    for (int i = 0; i < analysis.nodes.size(); ++i) {
        if (!analysis.nodes[i].id.isEmpty())
            byId.insert(analysis.nodes[i].id, i);
        if (analysis.nodes[i].elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0) {
            rootIndices.append(i);
            if (!analysis.nodes[i].id.isEmpty()) {
                rootIds.insert(analysis.nodes[i].id.toLower());
                rootIdLengths.insert(analysis.nodes[i].id.size());
            }
        }
    }
    QHash<QString, QSet<int>> actorSeedsByRoot;
    QHash<QString, QSet<int>> sameNameSeedsByRoot;
    for (int index = 0; index < analysis.nodes.size(); ++index) {
        const DataNode &node = analysis.nodes[index];
        if (node.elementName.startsWith(QStringLiteral("CActorUnit"), Qt::CaseInsensitive)) {
            const QString unitName = node.attributes.value(QStringLiteral("unitName")).toLower();
            if (rootIds.contains(unitName))
                actorSeedsByRoot[unitName].insert(index);
            for (const QString &reference : node.referencedIds) {
                const QString key = reference.toLower();
                if (rootIds.contains(key))
                    actorSeedsByRoot[key].insert(index);
            }
        }
        if (!isCatalogType(node.elementName) || node.id.isEmpty())
            continue;
        const QString id = node.id.toLower();
        for (int length : rootIdLengths) {
            if (length > id.size())
                continue;
            const QString prefix = id.left(length);
            if (rootIds.contains(prefix))
                sameNameSeedsByRoot[prefix].insert(index);
            const QString suffix = id.right(length);
            if (rootIds.contains(suffix))
                sameNameSeedsByRoot[suffix].insert(index);
        }
    }
    QVector<UnitFamily> families;
    for (int rootIndex : rootIndices) {
        const DataNode &root = analysis.nodes[rootIndex];
        UnitFamily family;
        family.rootNodeIndex = rootIndex;
        family.rootId = root.id;
        QSet<int> included{rootIndex};
        QHash<int, QString> evidence;
        evidence[rootIndex] = QStringLiteral("Root CUnit");

        // Proven inbound actor relation and same-root names establish seeds.
        const QString rootKey = root.id.toLower();
        for (int i : sameNameSeedsByRoot.value(rootKey)) {
            if (i == rootIndex) continue;
            included.insert(i);
            evidence[i] = QStringLiteral("Name similarity only (same-root naming)");
        }
        for (int i : actorSeedsByRoot.value(rootKey)) {
            if (i == rootIndex) continue;
            included.insert(i);
            evidence[i] = QStringLiteral("Proven actor field references root unit");
        }

        // Follow outgoing graph edges from root and established family objects.
        QQueue<QPair<int, int>> queue;
        for (int seed : included) queue.enqueue({seed, 0});
        while (!queue.isEmpty()) {
            const auto [index, depth] = queue.dequeue();
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

QVector<UnitFamily> UnitFamilyDetector::detectCollectionFamilies(const AnalysisResult &analysis, DataCollectionMode mode) const
{
    traceCollectionDetect(QStringLiteral("start"),
                          QStringLiteral("nodes=%1 mode=%2").arg(analysis.nodes.size()).arg(int(mode)));
    DataCollectionAliasMapper mapper;
    QHash<QString, QVector<int>> indicesByPrefix;
    QHash<QString, int> nodeByRecord;
    QHash<QString, const DataNode *> existingCollections;

    for (int index = 0; index < analysis.nodes.size(); ++index) {
        const DataNode &node = analysis.nodes[index];
        const QString catalog = mapper.catalogType(node.elementName);
        if (!catalog.isEmpty() && !node.id.isEmpty()) {
            nodeByRecord.insert(catalog.toLower() + QChar(0x1f) + node.id.toLower(), index);
            const int separator = node.id.indexOf(QLatin1Char('@'));
            if (separator > 0) indicesByPrefix[node.id.left(separator)].append(index);
        }
        if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
            && !node.elementName.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive)
            && !node.id.isEmpty())
            existingCollections.insert(node.id, &node);
    }
    traceCollectionDetect(QStringLiteral("base_index_done"),
                          QStringLiteral("existing_collections=%1").arg(existingCollections.size()));

    if (mode == DataCollectionMode::UnitAbilWeapon) {
        // Entity-root mode is deliberately graph-only.  Legacy DataRecord membership
        // and similar-looking IDs are observations for migration/review, never proof
        // that an object belongs to a gameplay entity.
        const auto entityTypeOf = [](const DataNode &node, DataCollectionEntityType *type) {
            if (node.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0) {
                *type = DataCollectionEntityType::Unit; return true;
            }
            if (node.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive)) {
                *type = DataCollectionEntityType::Ability; return true;
            }
            if (node.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive)) {
                *type = DataCollectionEntityType::Weapon; return true;
            }
            return false;
        };
        const auto allowedFor = [&](DataCollectionEntityType owner, const DataNode &node) {
            DataCollectionEntityType targetType;
            if (entityTypeOf(node, &targetType)) return false; // another root is a hard boundary
            const QString type = node.elementName.toLower();
            if (type.startsWith(QStringLiteral("cdatacollection")) || type.startsWith(QStringLiteral("cupgrade")))
                return false;
            if (owner == DataCollectionEntityType::Unit)
                return type.startsWith(QStringLiteral("cactor")) || type.startsWith(QStringLiteral("cmodel"))
                    || type.startsWith(QStringLiteral("csound")) || type.startsWith(QStringLiteral("cbutton"))
                    || type.startsWith(QStringLiteral("ceffect")) || type.startsWith(QStringLiteral("cbehavior"))
                    || type.startsWith(QStringLiteral("cvalidator")) || type.startsWith(QStringLiteral("crequirement"))
                    || type.startsWith(QStringLiteral("cmover")) || type.startsWith(QStringLiteral("cturret"))
                    || type.startsWith(QStringLiteral("cfootprint")) || type.startsWith(QStringLiteral("ctexture"))
                    || type.startsWith(QStringLiteral("csiteop")) || type.startsWith(QStringLiteral("cbeam"));
            if (owner == DataCollectionEntityType::Ability)
                return type.startsWith(QStringLiteral("cbutton")) || type.startsWith(QStringLiteral("ceffect"))
                    || type.startsWith(QStringLiteral("cbehavior")) || type.startsWith(QStringLiteral("cvalidator"))
                    || type.startsWith(QStringLiteral("crequirement")) || type.startsWith(QStringLiteral("cmover"))
                    || type.startsWith(QStringLiteral("cactor")) || type.startsWith(QStringLiteral("cmodel"))
                    || type.startsWith(QStringLiteral("csound")) || type.startsWith(QStringLiteral("csiteop"))
                    || type.startsWith(QStringLiteral("cbeam"));
            return type.startsWith(QStringLiteral("ceffect")) || type.startsWith(QStringLiteral("cactor"))
                || type.startsWith(QStringLiteral("cmodel")) || type.startsWith(QStringLiteral("csound"))
                || type.startsWith(QStringLiteral("cmover")) || type.startsWith(QStringLiteral("cvalidator"))
                || type.startsWith(QStringLiteral("csiteop")) || type.startsWith(QStringLiteral("cbeam"));
        };

        QHash<QString, QVector<int>> catalogById;
        QHash<QString, QVector<int>> incomingSourcesById;
        QHash<QString, QVector<int>> scopedCandidatesByScope;
        const auto addScopedCandidate = [&scopedCandidatesByScope](const QString &scope, int index) {
            if (scope.isEmpty())
                return;
            scopedCandidatesByScope[scope.toLower()].append(index);
        };
        QVector<int> roots;
        QHash<QString, QSet<int>> rootKinds;
        QSet<QString> primaryRootIds;
        for (int index = 0; index < analysis.nodes.size(); ++index) {
            const DataNode &node = analysis.nodes[index];
            const QString catalogType = mapper.catalogType(node.elementName);
            if (!node.id.isEmpty() && !catalogType.isEmpty()) {
                catalogById[node.id.toLower()].append(index);
                addScopedCandidate(node.id, index);
                for (int position = 0; position < node.id.size(); ++position) {
                    const QChar value = node.id.at(position);
                    if (position > 0 && (value == QLatin1Char('@') || value == QLatin1Char('_')))
                        addScopedCandidate(node.id.left(position), index);
                }
            }
            for (const QString &reference : node.referencedIds)
                incomingSourcesById[reference.toLower()].append(index);
            DataCollectionEntityType entityType;
            if (!node.id.isEmpty() && entityTypeOf(node, &entityType)) {
                roots.append(index);
                rootKinds[node.id.toLower()].insert(int(entityType));
                primaryRootIds.insert(node.id.toLower());
            }
            if (!node.id.isEmpty() && node.elementName.startsWith(QStringLiteral("CUpgrade"), Qt::CaseInsensitive))
                primaryRootIds.insert(node.id.toLower());
        }
        const auto scopedToForeignPrimary = [&](const QString &id, DataCollectionEntityType ownerType) {
            const QString idLower = id.toLower();
            QString bestRoot;
            for (const QString &primaryRootId : primaryRootIds) {
                if (primaryRootId.isEmpty() || !idLower.startsWith(primaryRootId))
                    continue;
                if (primaryRootId.size() > bestRoot.size())
                    bestRoot = primaryRootId;
            }
            if (bestRoot.isEmpty())
                return false;
            const QSet<int> kinds = rootKinds.value(bestRoot);
            return !kinds.contains(int(ownerType));
        };
        traceCollectionDetect(QStringLiteral("graph_index_done"),
                              QStringLiteral("roots=%1 catalog_ids=%2 incoming_ids=%3 scopes=%4")
                                  .arg(roots.size())
                                  .arg(catalogById.size())
                                  .arg(incomingSourcesById.size())
                                  .arg(scopedCandidatesByScope.size()));

        QVector<UnitFamily> result;
        const auto acceptsIncomingLinked = [](const DataNode &node) {
            const QString sourceType = node.elementName.toLower();
            return sourceType.startsWith(QStringLiteral("cactor"))
                || sourceType.startsWith(QStringLiteral("cmodel"))
                || sourceType.startsWith(QStringLiteral("csound"))
                || sourceType.startsWith(QStringLiteral("ceffect"))
                || sourceType.startsWith(QStringLiteral("cbehavior"));
        };
        const auto mergeOwners = [](QVector<quint64> *target, const QVector<quint64> &source) {
            bool changed = false;
            for (int word = 0; word < target->size(); ++word) {
                const quint64 added = source.at(word) & ~target->at(word);
                if (!added)
                    continue;
                (*target)[word] |= source.at(word);
                changed = true;
            }
            return changed;
        };
        const auto setOwner = [](QVector<quint64> *target, int owner) {
            (*target)[owner / 64] |= (quint64(1) << (owner % 64));
        };
        const auto hasOwner = [](const QVector<quint64> &bits, int owner) {
            return (bits.at(owner / 64) & (quint64(1) << (owner % 64))) != 0;
        };
        constexpr int kMaxOwnersForUnscopedPropagation = 64;

        const auto buildForEntity = [&](DataCollectionEntityType entityType) {
            QVector<int> typedRoots;
            for (int rootIndex : roots) {
                DataCollectionEntityType rootType;
                if (entityTypeOf(analysis.nodes[rootIndex], &rootType) && rootType == entityType)
                    typedRoots.append(rootIndex);
            }
            traceCollectionDetect(QStringLiteral("entity_roots"),
                                  QStringLiteral("entity=%1 roots=%2")
                                      .arg(dataCollectionEntityTypeName(entityType))
                                      .arg(typedRoots.size()));
            if (typedRoots.isEmpty())
                return;

            QVector<QVector<int>> adjacency(analysis.nodes.size());
            qsizetype edgeCount = 0;
            for (int index = 0; index < analysis.nodes.size(); ++index) {
                const DataNode &node = analysis.nodes[index];
                for (const QString &reference : node.referencedIds) {
                    for (int target : catalogById.value(reference.toLower())) {
                        if (target != index && allowedFor(entityType, analysis.nodes[target]))
                            adjacency[index].append(target);
                    }
                }
                if (!node.id.isEmpty()) {
                    const QString key = node.id.toLower();
                    for (int source : incomingSourcesById.value(key)) {
                        if (source != index && allowedFor(entityType, analysis.nodes[source])
                            && acceptsIncomingLinked(analysis.nodes[source]))
                            adjacency[index].append(source);
                    }
                    for (int candidate : scopedCandidatesByScope.value(key)) {
                        if (candidate != index && allowedFor(entityType, analysis.nodes[candidate]))
                            adjacency[index].append(candidate);
                    }
                }
                auto &edges = adjacency[index];
                std::sort(edges.begin(), edges.end());
                edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
                edgeCount += edges.size();
            }
            traceCollectionDetect(QStringLiteral("adjacency_done"),
                                  QStringLiteral("entity=%1 edges=%2")
                                      .arg(dataCollectionEntityTypeName(entityType))
                                      .arg(edgeCount));

            const int wordCount = (typedRoots.size() + 63) / 64;
            QVector<QVector<quint64>> owners(analysis.nodes.size(), QVector<quint64>(wordCount, 0));
            QVector<char> queued(analysis.nodes.size(), 0);
            QQueue<int> queue;
            for (int owner = 0; owner < typedRoots.size(); ++owner) {
                const int rootIndex = typedRoots.at(owner);
                setOwner(&owners[rootIndex], owner);
                if (!queued[rootIndex]) {
                    queued[rootIndex] = 1;
                    queue.enqueue(rootIndex);
                }
            }

            while (!queue.isEmpty()) {
                const int current = queue.dequeue();
                queued[current] = 0;
                QVector<quint64> currentOwners = owners.at(current);
                int currentOwnerCount = 0;
                for (int owner = 0; owner < typedRoots.size(); ++owner)
                    if (hasOwner(currentOwners, owner))
                        ++currentOwnerCount;
                if (currentOwnerCount > kMaxOwnersForUnscopedPropagation) {
                    QVector<quint64> scopedOwners(currentOwners.size(), 0);
                    const QString objectId = analysis.nodes[current].id;
                    for (int owner = 0; owner < typedRoots.size(); ++owner) {
                        if (!hasOwner(currentOwners, owner))
                            continue;
                        const int rootIndex = typedRoots.at(owner);
                        const QString root = analysis.nodes[rootIndex].id;
                        const bool canPropagate = current == rootIndex
                            || objectId.compare(root, Qt::CaseInsensitive) == 0
                            || objectId.startsWith(root + QLatin1Char('@'), Qt::CaseInsensitive)
                            || objectId.startsWith(root + QLatin1Char('_'), Qt::CaseInsensitive);
                        if (canPropagate)
                            setOwner(&scopedOwners, owner);
                    }
                    currentOwners = scopedOwners;
                }
                for (int target : adjacency.at(current)) {
                    if (!mergeOwners(&owners[target], currentOwners))
                        continue;
                    if (!queued[target]) {
                        queued[target] = 1;
                        queue.enqueue(target);
                    }
                }
            }
            traceCollectionDetect(QStringLiteral("propagation_done"),
                                  QStringLiteral("entity=%1")
                                      .arg(dataCollectionEntityTypeName(entityType)));

            for (int owner = 0; owner < typedRoots.size(); ++owner) {
                const int rootIndex = typedRoots.at(owner);
                const DataNode &rootNode = analysis.nodes[rootIndex];
                const DataNode *legacyCollection = existingCollections.value(rootNode.id, nullptr);
                if (!legacyCollection)
                    continue;
                pugi::xml_document fragment;
                if (!fragment.load_string(legacyCollection->serializedXml.toUtf8().constData()))
                    continue;
                for (pugi::xml_node record : fragment.first_child().children("DataRecord")) {
                    const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                    const QString catalog = entry.section(QLatin1Char(','), 0, 0).trimmed();
                    const QString id = entry.section(QLatin1Char(','), 1).trimmed();
                    const int target = nodeByRecord.value(catalog.toLower() + QChar(0x1f) + id.toLower(), -1);
                    if (target >= 0 && allowedFor(entityType, analysis.nodes[target])
                        && !scopedToForeignPrimary(analysis.nodes[target].id, entityType))
                        setOwner(&owners[target], owner);
                }
            }

            QVector<QVector<int>> membersByOwner(typedRoots.size());
            QVector<char> sharedUnscoped(analysis.nodes.size(), 0);
            qsizetype rawMemberships = 0;
            qsizetype prunedMemberships = 0;
            for (int index = 0; index < owners.size(); ++index) {
                const QVector<quint64> &bits = owners.at(index);
                if (bits.isEmpty())
                    continue;
                const QString objectId = analysis.nodes[index].id;
                if (scopedToForeignPrimary(objectId, entityType))
                    continue;
                int scopedOwner = -1;
                int longestScope = -1;
                bool ambiguousScopedOwner = false;
                QVector<int> ownerIndices;
                ownerIndices.reserve(8);
                for (int owner = 0; owner < typedRoots.size(); ++owner) {
                    if (!hasOwner(bits, owner))
                        continue;
                    ownerIndices.append(owner);
                    ++rawMemberships;
                    const QString root = analysis.nodes[typedRoots.at(owner)].id;
                    const bool scoped = objectId.compare(root, Qt::CaseInsensitive) == 0
                        || objectId.startsWith(root + QLatin1Char('@'), Qt::CaseInsensitive)
                        || objectId.startsWith(root + QLatin1Char('_'), Qt::CaseInsensitive);
                    if (!scoped)
                        continue;
                    if (root.size() > longestScope) {
                        scopedOwner = owner;
                        longestScope = root.size();
                        ambiguousScopedOwner = false;
                    } else if (root.size() == longestScope) {
                        ambiguousScopedOwner = true;
                    }
                }
                if (ownerIndices.isEmpty())
                    continue;
                if (scopedOwner >= 0 && !ambiguousScopedOwner) {
                    membersByOwner[scopedOwner].append(index);
                    ++prunedMemberships;
                    continue;
                }
                if (ownerIndices.size() > 1)
                    sharedUnscoped[index] = 1;
                for (int owner : ownerIndices) {
                    membersByOwner[owner].append(index);
                    ++prunedMemberships;
                }
            }
            traceCollectionDetect(QStringLiteral("ownership_pruned"),
                                  QStringLiteral("entity=%1 raw=%2 kept=%3")
                                      .arg(dataCollectionEntityTypeName(entityType))
                                      .arg(rawMemberships)
                                      .arg(prunedMemberships));

            for (int owner = 0; owner < typedRoots.size(); ++owner) {
                const int rootIndex = typedRoots.at(owner);
                const DataNode &rootNode = analysis.nodes[rootIndex];
                UnitFamily family;
                family.rootId = rootNode.id;
                family.rootNodeIndex = rootIndex;
                family.entityType = entityType;
                family.collectionElementName = entityType == DataCollectionEntityType::Unit ? QStringLiteral("CDataCollectionUnit")
                    : entityType == DataCollectionEntityType::Ability ? QStringLiteral("CDataCollectionAbil")
                                                                      : QStringLiteral("CDataCollection");
                family.strictOwnership = true;
                family.rootTypeConflict = rootKinds.value(rootNode.id.toLower()).size() > 1;

                QSet<int> members;
                for (int index : membersByOwner[owner])
                    members.insert(index);
                family.recommendedParent = standardDataCollectionParent(entityType, members, analysis.nodes);

                QList<int> sortedMembers = members.values();
                std::sort(sortedMembers.begin(), sortedMembers.end());
                for (int index : sortedMembers) {
                    QString confidence = QStringLiteral("High");
                    QString reason = index == rootIndex
                        ? QStringLiteral("Real typed gameplay root")
                        : QStringLiteral("XML reference graph reachability");
                    if (index != rootIndex && sharedUnscoped.value(index)) {
                        confidence = QStringLiteral("Shared");
                        reason += QStringLiteral("; reached from multiple entity roots");
                    }
                    UnitFamilyRole role = roleFromNode(analysis.nodes[index], family.rootId, &confidence, &reason);
                    if (role == UnitFamilyRole::ManualReview) role = UnitFamilyRole::Other;
                    family.objects.append({index, role, confidence, reason});
                }
                result.append(family);
            }
            traceCollectionDetect(QStringLiteral("families_built"),
                                  QStringLiteral("entity=%1 total=%2")
                                      .arg(dataCollectionEntityTypeName(entityType))
                                      .arg(result.size()));
        };

        buildForEntity(DataCollectionEntityType::Unit);
        buildForEntity(DataCollectionEntityType::Ability);
        buildForEntity(DataCollectionEntityType::Weapon);
        traceCollectionDetect(QStringLiteral("all_entities_done"),
                              QStringLiteral("families=%1").arg(result.size()));
        // No-loss collection generation is many-to-many by design.  If a shared
        // catalog object is reachable from several roots, keep it in every
        // reachable collection instead of doing an expensive single-owner pass.
        std::sort(result.begin(), result.end(), [](const UnitFamily &left, const UnitFamily &right) {
            const int idOrder = left.rootId.compare(right.rootId, Qt::CaseInsensitive);
            return idOrder != 0 ? idOrder < 0 : int(left.entityType) < int(right.entityType);
        });
        traceCollectionDetect(QStringLiteral("done"),
                              QStringLiteral("families=%1").arg(result.size()));
        return result;

#if 0 // Previous heuristic splitter retained temporarily for history; graph-only logic above is authoritative.
        const auto isTypedRoot = [](const DataNode &node) {
            return node.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0
                || node.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive)
                || node.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive)
                || node.elementName.startsWith(QStringLiteral("CUpgrade"), Qt::CaseInsensitive);
        };
        const auto rootPriority = [](const DataNode &node) {
            if (node.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0) return 0;
            if (node.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive)) return 1;
            if (node.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive)) return 2;
            return 3;
        };
        QHash<QString, QVector<int>> catalogById;
        for (int index = 0; index < analysis.nodes.size(); ++index) {
            const DataNode &node = analysis.nodes[index];
            if (!mapper.catalogType(node.elementName).isEmpty() && !node.id.isEmpty())
                catalogById[node.id.toLower()].append(index);
        }

        QHash<QString, QSet<int>> objectsByRoot;
        QHash<QString, int> rootIndexById;
        QSet<int> consumed;

        const auto addGroup = [&](QSet<int> universe, const QString &preferredRoot) {
            if (universe.isEmpty()) return;
            QVector<int> roots;
            for (int index : universe) {
                if (index >= 0 && index < analysis.nodes.size() && isTypedRoot(analysis.nodes[index])
                    && !analysis.nodes[index].id.contains(QLatin1Char('@')))
                    roots.append(index);
            }
            std::sort(roots.begin(), roots.end(), [&](int left, int right) {
                const int lp = rootPriority(analysis.nodes[left]), rp = rootPriority(analysis.nodes[right]);
                if (lp != rp) return lp < rp;
                return analysis.nodes[left].id.compare(analysis.nodes[right].id, Qt::CaseInsensitive) < 0;
            });
            roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
            if (roots.isEmpty()) return;

            // A complete legacy collection is the authoritative universe.  Assign every
            // record to exactly one typed root instead of silently dropping non-prefixed IDs.
            QHash<int, QHash<int, int>> distances;
            for (int rootIndex : roots) {
                QQueue<int> queue;
                distances[rootIndex].insert(rootIndex, 0);
                queue.enqueue(rootIndex);
                while (!queue.isEmpty()) {
                    const int current = queue.dequeue();
                    const int nextDistance = distances[rootIndex].value(current) + 1;
                    for (const QString &reference : analysis.nodes[current].referencedIds) {
                        for (int target : catalogById.value(reference.toLower())) {
                            if (!universe.contains(target) || distances[rootIndex].contains(target)) continue;
                            if (target != rootIndex && roots.contains(target)) continue;
                            distances[rootIndex].insert(target, nextDistance);
                            queue.enqueue(target);
                        }
                    }
                }
            }

            int fallbackRoot = roots.front();
            for (int rootIndex : roots) {
                const DataNode &rootNode = analysis.nodes[rootIndex];
                if (rootNode.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0
                    || rootNode.id.compare(preferredRoot, Qt::CaseInsensitive) == 0) {
                    fallbackRoot = rootIndex;
                    if (rootNode.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0) break;
                }
            }

            for (int index : universe) {
                QVector<int> owners;
                if (roots.contains(index)) {
                    owners.append(index);
                } else for (int rootIndex : roots) {
                    const QString id = analysis.nodes[index].id.toLower();
                    const QString root = analysis.nodes[rootIndex].id.toLower();
                    const bool namedForRoot = id.startsWith(root + QLatin1Char('@'))
                        || id.startsWith(root + QLatin1Char('_'))
                        || (root.size() >= 4 && (id.startsWith(root) || id.endsWith(root)));
                    const bool graphRelated = distances[rootIndex].contains(index);
                    if (namedForRoot || graphRelated) owners.append(rootIndex);
                }
                // Shared requirements, effects and upgrades intentionally remain in
                // every typed collection that reaches them. Data Collection membership
                // is many-to-many; reducing it to one owner loses copy relationships.
                if (owners.isEmpty()) owners.append(fallbackRoot);
                for (int owner : owners) {
                    const QString ownerId = analysis.nodes[owner].id;
                    objectsByRoot[ownerId].insert(index);
                    rootIndexById.insert(ownerId, owner);
                }
            }
            consumed.unite(universe);
        };

        const QVector<UnitFamily> detected = detect(analysis);

        // Parse all existing DataRecord lists first.  This preserves the complete
        // 7k-style family even when its real IDs use old/non-standard names.
        for (auto it = existingCollections.cbegin(); it != existingCollections.cend(); ++it) {
            pugi::xml_document fragment;
            if (!fragment.load_string(it.value()->serializedXml.toUtf8().constData())) continue;
            QSet<int> universe;
            for (pugi::xml_node record : fragment.first_child().children("DataRecord")) {
                const QString entry = QString::fromUtf8(record.attribute("Entry").value());
                const QString catalog = entry.section(QLatin1Char(','), 0, 0).trimmed();
                const QString id = entry.section(QLatin1Char(','), 1).trimmed();
                const int index = nodeByRecord.value(catalog.toLower() + QChar(0x1f) + id.toLower(), -1);
                if (index >= 0) universe.insert(index);
            }

            // Recover records omitted by an earlier incomplete split from the real
            // unit graph and from standard Root@Child / Root_Child naming.
            for (const UnitFamily &candidate : detected) {
                if (candidate.rootId.compare(it.key(), Qt::CaseInsensitive) != 0) continue;
                for (const UnitFamilyObject &object : candidate.objects)
                    if (object.nodeIndex >= 0 && !mapper.catalogType(analysis.nodes[object.nodeIndex].elementName).isEmpty())
                        universe.insert(object.nodeIndex);
                break;
            }
            QStringList typedNames;
            for (int index : universe)
                if (isTypedRoot(analysis.nodes[index])) typedNames.append(analysis.nodes[index].id);
            for (int index = 0; index < analysis.nodes.size(); ++index) {
                if (mapper.catalogType(analysis.nodes[index].elementName).isEmpty()) continue;
                for (const QString &typedName : typedNames) {
                    if (analysis.nodes[index].id.startsWith(typedName + QLatin1Char('@'), Qt::CaseInsensitive)
                        || analysis.nodes[index].id.startsWith(typedName + QLatin1Char('_'), Qt::CaseInsensitive)) {
                        universe.insert(index);
                        break;
                    }
                }
            }
            QQueue<int> recoveryQueue;
            for (int index : universe) recoveryQueue.enqueue(index);
            while (!recoveryQueue.isEmpty()) {
                const int current = recoveryQueue.dequeue();
                for (const QString &reference : analysis.nodes[current].referencedIds) {
                    for (int target : catalogById.value(reference.toLower())) {
                        if (universe.contains(target)) continue;
                        if (analysis.nodes[target].elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0)
                            continue;
                        universe.insert(target);
                        recoveryQueue.enqueue(target);
                    }
                }
            }
            addGroup(universe, it.key());
        }

        // Maps without a complete collection still get graph-derived groups.
        // Unit groups are processed first so their ability/weapon/upgrade roots are split together.
        for (const UnitFamily &family : detected) {
            if (consumed.contains(family.rootNodeIndex)) continue;
            QSet<int> universe;
            for (const UnitFamilyObject &object : family.objects)
                if (object.nodeIndex >= 0 && mapper.catalogType(analysis.nodes[object.nodeIndex].elementName).size())
                    universe.insert(object.nodeIndex);
            addGroup(universe, family.rootId);
        }
        for (int index = 0; index < analysis.nodes.size(); ++index) {
            const DataNode &node = analysis.nodes[index];
            if (consumed.contains(index) || !isTypedRoot(node) || node.id.isEmpty() || node.id.contains(QLatin1Char('@')))
                continue;
            QSet<int> universe{index};
            QQueue<int> queue;
            queue.enqueue(index);
            while (!queue.isEmpty()) {
                const int current = queue.dequeue();
                for (const QString &reference : analysis.nodes[current].referencedIds) {
                    for (int target : catalogById.value(reference.toLower())) {
                        if (consumed.contains(target) || universe.contains(target)) continue;
                        if (target != index && isTypedRoot(analysis.nodes[target]) && !analysis.nodes[target].id.contains(QLatin1Char('@')))
                            continue;
                        universe.insert(target);
                        queue.enqueue(target);
                    }
                }
            }
            addGroup(universe, node.id);
        }

        QStringList roots = objectsByRoot.keys();
        std::sort(roots.begin(), roots.end(), [](const QString &left, const QString &right) {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        });
        QVector<UnitFamily> result;
        for (const QString &root : roots) {
            UnitFamily family;
            family.rootId = root;
            family.rootNodeIndex = rootIndexById.value(root, -1);
            family.collectionElementName = QStringLiteral("CDataCollectionUnit");
            family.strictOwnership = true;
            family.recommendedParent.clear();
            QList<int> indices = objectsByRoot.value(root).values();
            std::sort(indices.begin(), indices.end());
            for (int index : indices) {
                QString confidence = index == family.rootNodeIndex ? QStringLiteral("High") : QStringLiteral("Medium");
                QString reason = index == family.rootNodeIndex
                    ? QStringLiteral("Typed collection root")
                    : QStringLiteral("Assigned from complete collection membership and reference graph");
                UnitFamilyRole role = roleFromNode(analysis.nodes[index], root, &confidence, &reason);
                if (role == UnitFamilyRole::ManualReview) {
                    role = UnitFamilyRole::Other;
                    confidence = QStringLiteral("Medium");
                    reason += QStringLiteral("; catalog membership is authoritative even though the semantic role is generic");
                }
                family.objects.append({index, role, confidence, reason});
            }
            result.append(family);
        }
        return result;
#endif
    }

    const QVector<UnitFamily> detectedFamilies = detect(analysis);
    if (mode == DataCollectionMode::Unit) for (const UnitFamily &family : detectedFamilies) {
        if (family.rootId.isEmpty() || family.rootId.contains(QLatin1Char('@'))
            || indicesByPrefix.contains(family.rootId) || existingCollections.contains(family.rootId))
            continue;

        QVector<int> indices;
        for (const UnitFamilyObject &object : family.objects) {
            if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size())
                continue;
            const DataNode &node = analysis.nodes[object.nodeIndex];
            if (mapper.catalogType(node.elementName).isEmpty())
                continue;
            if (object.role == UnitFamilyRole::ManualReview)
                continue;
            indices.append(object.nodeIndex);
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.size() >= 2)
            indicesByPrefix.insert(family.rootId, indices);
    }

    // A map-defined root catalog object is useful as a Data Collection even
    // when no related @ children can be proven.
    for (int index = 0; index < analysis.nodes.size(); ++index) {
        const DataNode &node = analysis.nodes[index];
        const bool unit = node.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0;
        const bool ability = node.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive);
        const bool weapon = node.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive);
        if ((!unit && (mode != DataCollectionMode::UnitAbilWeapon || (!ability && !weapon)))
            || node.id.isEmpty() || node.id.contains(QLatin1Char('@'))
            || existingCollections.contains(node.id))
            continue;
        QVector<int> &indices = indicesByPrefix[node.id];
        if (!indices.contains(index)) indices.prepend(index);
    }

    // Existing collections are supplemented from their real DataRecord
    // entries. The referenced ID must already exist in the analyzed catalog.
    for (auto it = existingCollections.cbegin(); it != existingCollections.cend(); ++it) {
        pugi::xml_document fragment;
        if (!fragment.load_string(it.value()->serializedXml.toUtf8().constData())) continue;
        for (pugi::xml_node record : fragment.first_child().children("DataRecord")) {
            const QString entry = QString::fromUtf8(record.attribute("Entry").value());
            const QString catalog = entry.section(QLatin1Char(','), 0, 0).trimmed();
            const QString id = entry.section(QLatin1Char(','), 1).trimmed();
            if (mode == DataCollectionMode::UnitAbilWeapon
                && id.compare(it.key(), Qt::CaseInsensitive) != 0
                && !id.startsWith(it.key() + QLatin1Char('@'), Qt::CaseInsensitive))
                continue;
            const int index = nodeByRecord.value(catalog.toLower() + QChar(0x1f) + id.toLower(), -1);
            if (index >= 0 && !indicesByPrefix[it.key()].contains(index)) indicesByPrefix[it.key()].append(index);
        }
    }

    QStringList roots = indicesByPrefix.keys();
    std::sort(roots.begin(), roots.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    QVector<UnitFamily> result;
    for (const QString &root : roots) {
        QVector<int> indices = indicesByPrefix.value(root);
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        const bool existing = existingCollections.contains(root);
        if (indices.isEmpty()) continue;

        UnitFamily family;
        family.rootId = root;
        family.rootNodeIndex = indices.front();
        if (existing) {
            family.collectionElementName = existingCollections.value(root)->elementName;
        } else {
            bool hasUnit = false;
            bool allAbilities = true;
            bool allUpgrades = true;
            for (int index : indices) {
                const QString type = analysis.nodes[index].elementName;
                hasUnit = hasUnit || type.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0;
                allAbilities = allAbilities && type.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive);
                allUpgrades = allUpgrades && type.startsWith(QStringLiteral("CUpgrade"), Qt::CaseInsensitive);
                if (type.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0) family.rootNodeIndex = index;
            }
            family.collectionElementName = hasUnit ? QStringLiteral("CDataCollectionUnit")
                : allAbilities ? QStringLiteral("CDataCollectionAbil")
                : allUpgrades ? QStringLiteral("CDataCollectionUpgrade")
                              : QStringLiteral("CDataCollection");
        }

        if (mode == DataCollectionMode::UnitAbilWeapon) {
            family.collectionElementName = QStringLiteral("CDataCollectionUnit");
            family.strictOwnership = true;
            family.recommendedParent.clear();
        }

        for (int index : indices) {
            QString confidence = QStringLiteral("High");
            QString reason = QStringLiteral("Real object ID starts with CollectionID@");
            const UnitFamilyRole role = roleFromNode(analysis.nodes[index], root, &confidence, &reason);
            family.objects.append({index, role, confidence, reason});
        }
        result.append(family);
    }
    return result;
}
