#include "core/DataCollectionUnitBuilder.h"

#include "core/BackupManager.h"
#include "core/DataCollectionPreservation.h"
#include "core/FolderAnalyzer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <pugixml.hpp>

#include <algorithm>
#include <sstream>

namespace {

bool readBytes(const QString &path, QByteArray *bytes, QString *error);

int recordOrder(const QString &entry)
{
    static const QStringList order{QStringLiteral("Button"), QStringLiteral("Unit"), QStringLiteral("Actor"),
        QStringLiteral("Model"), QStringLiteral("Sound"), QStringLiteral("Weapon"), QStringLiteral("Abil"),
        QStringLiteral("Effect"), QStringLiteral("Behavior"), QStringLiteral("Validator"),
        QStringLiteral("Requirement"), QStringLiteral("Upgrade")};
    const QString catalog = entry.section(QLatin1Char(','), 0, 0);
    const int index = order.indexOf(catalog);
    return index < 0 ? order.size() : index;
}

void sortEntries(QStringList *entries)
{
    std::sort(entries->begin(), entries->end(), [](const QString &left, const QString &right) {
        const int leftOrder = recordOrder(left), rightOrder = recordOrder(right);
        if (leftOrder != rightOrder) return leftOrder < rightOrder;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
}

pugi::xml_node findByTypeAndId(pugi::xml_document &doc, const QString &type, const QString &id)
{
    for (const pugi::xpath_node &match : doc.select_nodes("//*[@id]")) {
        const pugi::xml_node node = match.node();
        if (QString::fromUtf8(node.name()).compare(type, Qt::CaseInsensitive) == 0
            && QString::fromUtf8(node.attribute("id").value()) == id) return node;
    }
    return {};
}

pugi::xml_node findCollectionById(pugi::xml_document &doc, const QString &id)
{
    for (const pugi::xpath_node &match : doc.select_nodes("//*[@id]")) {
        const pugi::xml_node node = match.node();
        const QString type = QString::fromUtf8(node.name());
        if (type.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
            && !type.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive)
            && QString::fromUtf8(node.attribute("id").value()) == id) return node;
    }
    return {};
}

bool isDataCollectionNodeName(const QString &type)
{
    return type.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
        && !type.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive);
}

pugi::xml_node ensureByTypeAndId(pugi::xml_node catalog, const QString &type, const QString &id)
{
    for (pugi::xml_node node : catalog.children(type.toUtf8().constData())) {
        if (QString::fromUtf8(node.attribute("id").value()).compare(id, Qt::CaseInsensitive) == 0)
            return node;
    }
    pugi::xml_node node = catalog.prepend_child(type.toUtf8().constData());
    node.append_attribute("id").set_value(id.toUtf8().constData());
    return node;
}

pugi::xml_node retagCollection(pugi::xml_node catalog, pugi::xml_node oldCollection, const QString &newType)
{
    pugi::xml_node replacement = catalog.insert_child_before(newType.toUtf8().constData(), oldCollection);
    for (pugi::xml_attribute attribute : oldCollection.attributes())
        replacement.append_attribute(attribute.name()).set_value(attribute.value());
    for (pugi::xml_node child : oldCollection.children())
        replacement.append_copy(child);
    oldCollection.parent().remove_child(oldCollection);
    return replacement;
}

QString standardCollectionElementName(const QString &id)
{
    if (id.startsWith(QStringLiteral("Ability"), Qt::CaseInsensitive))
        return QStringLiteral("CDataCollectionAbil");
    if (id.startsWith(QStringLiteral("Weapon"), Qt::CaseInsensitive))
        return QStringLiteral("CDataCollection");
    return QStringLiteral("CDataCollectionUnit");
}

pugi::xml_node ensureCollectionTemplateById(pugi::xml_node catalog, const QString &id)
{
    const QString expectedType = standardCollectionElementName(id);
    pugi::xml_node firstMatch;
    pugi::xml_node typedMatch;

    for (pugi::xml_node node = catalog.first_child(); node; node = node.next_sibling()) {
        const QString type = QString::fromUtf8(node.name());
        if (!isDataCollectionNodeName(type))
            continue;
        if (QString::fromUtf8(node.attribute("id").value()).compare(id, Qt::CaseInsensitive) != 0)
            continue;
        if (!firstMatch)
            firstMatch = node;
        if (type.compare(expectedType, Qt::CaseInsensitive) == 0) {
            typedMatch = node;
            break;
        }
    }

    if (typedMatch)
        return typedMatch;
    if (firstMatch)
        return retagCollection(catalog, firstMatch, expectedType);

    pugi::xml_node node = catalog.prepend_child(expectedType.toUtf8().constData());
    node.append_attribute("id").set_value(id.toUtf8().constData());
    return node;
}

QStringList existingEntries(const pugi::xml_node &collection, QStringList *duplicates = nullptr)
{
    QStringList entries;
    QSet<QString> seen;
    for (pugi::xml_node record : collection.children("DataRecord")) {
        const QString entry = QString::fromUtf8(record.attribute("Entry").value());
        if (entry.isEmpty()) continue;
        if (seen.contains(entry)) { if (duplicates) duplicates->append(entry); }
        else { seen.insert(entry); entries.append(entry); }
    }
    return entries;
}

void setAttribute(pugi::xml_node node, const char *name, const QString &value)
{
    pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute) attribute = node.append_attribute(name);
    attribute.set_value(value.toUtf8().constData());
}

void updateCollection(pugi::xml_node collection, const QString &parent, const QString &categories,
                       const QStringList &records, bool pruneUnrelated)
{
    Q_UNUSED(pruneUnrelated);
    if (!parent.isEmpty()) setAttribute(collection, "parent", parent);
    pugi::xml_node category = collection.child("EditorCategories");
    if (!categories.isEmpty()) {
        if (!category) category = collection.prepend_child("EditorCategories");
        setAttribute(category, "value", categories);
    }
    QSet<QString> existing;
    for (pugi::xml_node record : collection.children("DataRecord")) {
        const QString entry = QString::fromUtf8(record.attribute("Entry").value());
        if (!entry.isEmpty())
            existing.insert(entry);
    }
    for (const QString &entry : records) {
        if (existing.contains(entry)) continue;
        pugi::xml_node record = collection.append_child("DataRecord");
        setAttribute(record, "Entry", entry);
        existing.insert(entry);
    }
}

QString serializeNode(const pugi::xml_node &node)
{
    std::ostringstream stream;
    node.print(stream, "  ", pugi::format_default, pugi::encoding_utf8);
    return QString::fromUtf8(stream.str());
}

QString defaultCategoriesFor(const DataNode &root)
{
    if (root.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive))
        return QStringLiteral("DataGroup:Ability,ObjectType:Other");
    if (root.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive))
        return QStringLiteral("DataGroup:Weapon,ObjectType:Other");
    return QStringLiteral("DataGroup:Unit,ObjectType:Unit");
}

bool idIsScopedToRoot(const QString &id, const QString &root)
{
    return id.compare(root, Qt::CaseInsensitive) == 0
        || id.startsWith(root + QLatin1Char('@'), Qt::CaseInsensitive)
        || id.startsWith(root + QLatin1Char('_'), Qt::CaseInsensitive);
}

int entityPriority(DataCollectionEntityType type)
{
    switch (type) {
    case DataCollectionEntityType::Unit: return 0;
    case DataCollectionEntityType::Ability: return 1;
    case DataCollectionEntityType::Weapon: return 2;
    }
    return 3;
}

int ownershipPathDepth(const QString &reason)
{
    if (reason.isEmpty())
        return 100000;
    return reason.count(QStringLiteral(" -> "));
}

bool isSharedManualObject(const UnitFamilyObject &object)
{
    return object.role == UnitFamilyRole::ManualReview
        && object.confidence.compare(QStringLiteral("Shared"), Qt::CaseInsensitive) == 0;
}

struct AliasOwnerCandidate
{
    QString rootId;
    DataCollectionEntityType entityType = DataCollectionEntityType::Unit;
    bool scoped = false;
    int pathDepth = 100000;
};

bool patternMatchesEntity(const QString &pattern, DataCollectionEntityType entity)
{
    const QString expected = entity == DataCollectionEntityType::Unit ? QStringLiteral("UnitPattern")
        : entity == DataCollectionEntityType::Ability ? QStringLiteral("AbilityPattern")
                                                      : QStringLiteral("WeaponPattern");
    return pattern.startsWith(expected, Qt::CaseInsensitive);
}

QHash<QString, QString> standardCollectionParents()
{
    return {
        {QStringLiteral("UnitBase"), QString()},
        {QStringLiteral("UnitGround"), QStringLiteral("UnitBase")},
        {QStringLiteral("UnitAir"), QStringLiteral("UnitBase")},
        {QStringLiteral("WeaponBase"), QString()},
        {QStringLiteral("Weapon_Instant"), QStringLiteral("WeaponBase")},
        {QStringLiteral("Weapon_Missile"), QStringLiteral("WeaponBase")},
        {QStringLiteral("AbilityBase"), QString()},
        {QStringLiteral("AbilityMisssile"), QStringLiteral("AbilityBase")},
    };
}

QHash<QString, QString> standardCollectionPatterns()
{
    return {
        {QStringLiteral("UnitBase"), QStringLiteral("UnitPattern_Base")},
        {QStringLiteral("UnitAir"), QStringLiteral("UnitPattern_Air")},
        {QStringLiteral("WeaponBase"), QStringLiteral("WeaponPattern_Base")},
        {QStringLiteral("Weapon_Missile"), QStringLiteral("WeaponPattern_Missile")},
        {QStringLiteral("AbilityBase"), QStringLiteral("AbilityPattern_Base")},
        {QStringLiteral("AbilityMisssile"), QStringLiteral("AbilityPattern_Missile")},
    };
}

QSet<QString> standardPatternIds()
{
    return {
        QStringLiteral("UnitPattern_Base").toLower(),
        QStringLiteral("UnitPattern_Air").toLower(),
        QStringLiteral("WeaponPattern_Base").toLower(),
        QStringLiteral("WeaponPattern_Missile").toLower(),
        QStringLiteral("AbilityPattern_Base").toLower(),
        QStringLiteral("AbilityPattern_Missile").toLower(),
    };
}

void ensurePatternField(pugi::xml_node pattern, const QString &reference, const QString &nameOverride = {})
{
    for (pugi::xml_node field : pattern.children("Fields")) {
        const QString existing = QString::fromUtf8(field.attribute("Reference").value());
        if (existing.compare(reference, Qt::CaseInsensitive) == 0)
            return;
    }
    pugi::xml_node field = pattern.append_child("Fields");
    setAttribute(field, "Reference", reference);
    if (!nameOverride.isEmpty())
        setAttribute(field, "NameOverride", nameOverride);
}

void ensureStandardPatternFields(pugi::xml_node pattern, const QString &id)
{
    if (id == QStringLiteral("UnitPattern_Base")) {
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Hotkey"));
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Name"));
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Tooltip"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,LifeStart"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,LifeMax"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,LifeArmor"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,ShieldsStart"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,ShieldsMax"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,ShieldArmor"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,EnergyStart"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,EnergyMax"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,EnergyArmor"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,Sight"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,Speed"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,Food"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,CostResource[Minerals]"),
                           QStringLiteral("DataCollectionPattern/NameOverride/CostResource/Minerals"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,CostResource[Vespene]"),
                           QStringLiteral("DataCollectionPattern/NameOverride/CostResource/Vespene"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,RepairTime"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,AbilArray"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,BehaviorArray"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,WeaponArray"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,CardLayouts"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,Race"));
        ensurePatternField(pattern, QStringLiteral("Actor,^ParamId^,UnitIcon"));
        ensurePatternField(pattern, QStringLiteral("Actor,^ParamId^,HeroIcon"));
        ensurePatternField(pattern, QStringLiteral("Actor,^ParamId^,LifeArmorIcon"));
        ensurePatternField(pattern, QStringLiteral("Actor,^ParamId^,Wireframe[0].Image[0]"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Wireframe"));
        ensurePatternField(pattern, QStringLiteral("Actor,^ParamId^,GroupIcon[0].Image[0]"),
                           QStringLiteral("DataCollectionPattern/NameOverride/GroupIcon"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^,Model"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@Death,Model"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Death"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@Portrait,Model"),
                           QStringLiteral("DataCollectionPattern/NameOverride/PortraitModel"));
        ensurePatternField(pattern, QStringLiteral("Sound,^ParamId^@Attack,AssetArray"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Sounds/Attack"));
        ensurePatternField(pattern, QStringLiteral("Sound,^ParamId^@Ready,AssetArray"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Sounds/Ready"));
        ensurePatternField(pattern, QStringLiteral("DataCollection,^ParamId^,TechInfoUpgradeUsed"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Upgrades"));
    } else if (id == QStringLiteral("UnitPattern_Air")) {
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,Height"));
        ensurePatternField(pattern, QStringLiteral("Unit,^ParamId^,VisionHeight"));
    } else if (id == QStringLiteral("WeaponPattern_Base")) {
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Name"));
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Tooltip"));
        ensurePatternField(pattern, QStringLiteral("Weapon,^ParamId^,Period"));
        ensurePatternField(pattern, QStringLiteral("Weapon,^ParamId^,Icon"));
        ensurePatternField(pattern, QStringLiteral("Weapon,^ParamId^,Range"));
        ensurePatternField(pattern, QStringLiteral("Weapon,^ParamId^,Arc"));
        ensurePatternField(pattern, QStringLiteral("Weapon,^ParamId^,AllowedMovement"));
        ensurePatternField(pattern, QStringLiteral("Weapon,^ParamId^,TargetFilters"));
        ensurePatternField(pattern, QStringLiteral("Effect,^ParamId^@Damage,Amount"));
        ensurePatternField(pattern, QStringLiteral("Actor,^ParamId^@Attack,LaunchAttachQuery"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@AttackImpact,Model"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Model/Impact"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@AttackLaunch,Model"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Model/Launch"));
        ensurePatternField(pattern, QStringLiteral("Sound,^ParamId^@AttackImpact,AssetArray"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Sound/Impact"));
        ensurePatternField(pattern, QStringLiteral("Sound,^ParamId^@AttackLaunch,AssetArray"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Sound/Launch"));
        ensurePatternField(pattern, QStringLiteral("DataCollection,^ParamId^,TechInfoUpgradeUsed"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Upgrades"));
        ensurePatternField(pattern, QStringLiteral("DataCollection,^ParamId^,UpgradeInfoWeapon[0]"),
                           QStringLiteral("DataCollectionPattern/NameOverride/UpgradeWeapon1"));
    } else if (id == QStringLiteral("WeaponPattern_Missile")) {
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@AttackMissile,Model"),
                           QStringLiteral("DataCollectionPattern/NameOverride/Model/Missile"));
        ensurePatternField(pattern, QStringLiteral("Mover,^ParamId^,MotionPhases"));
    } else if (id == QStringLiteral("AbilityPattern_Base")) {
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Name"));
        ensurePatternField(pattern, QStringLiteral("Button,^ParamId^,Tooltip"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,Cost.Cooldown.TimeUse"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,Cost.Cooldown.TimeStart"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,Cost.Vital[Energy]"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,TargetFilters"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,Range"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,AutoCastRange"));
        ensurePatternField(pattern, QStringLiteral("Abil,^ParamId^,CmdButtonArray"));
        ensurePatternField(pattern, QStringLiteral("Effect,^ParamId^@Damage,Amount"));
        ensurePatternField(pattern, QStringLiteral("Effect,^ParamId^@Set,EffectArray"));
        ensurePatternField(pattern, QStringLiteral("Behavior,^ParamId^,Modification"));
        ensurePatternField(pattern, QStringLiteral("Behavior,^ParamId^,DamageResponse"));
        ensurePatternField(pattern, QStringLiteral("Behavior,^ParamId^,Duration"));
    } else if (id == QStringLiteral("AbilityPattern_Missile")) {
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@AttackMissile,Model"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@AttackImpact,Model"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@AttackLaunch,Model"));
        ensurePatternField(pattern, QStringLiteral("Model,^ParamId^@ImpactModel,Model"));
        ensurePatternField(pattern, QStringLiteral("Sound,^ParamId^@AttackLaunch,AssetArray"));
        ensurePatternField(pattern, QStringLiteral("Sound,^ParamId^@AttackImpact,AssetArray"));
        ensurePatternField(pattern, QStringLiteral("Mover,^ParamId^,MotionPhases"));
    }
}

void ensureStandardDataCollectionSupport(pugi::xml_node catalog)
{
    const QVector<QPair<QString, QString>> patternNodes{
        {QStringLiteral("UnitPattern_Air"), QStringLiteral("UnitPattern_Base")},
        {QStringLiteral("WeaponPattern_Missile"), QStringLiteral("WeaponPattern_Base")},
        {QStringLiteral("AbilityPattern_Missile"), QStringLiteral("AbilityPattern_Base")},
        {QStringLiteral("AbilityPattern_Base"), QString()},
        {QStringLiteral("WeaponPattern_Base"), QString()},
        {QStringLiteral("UnitPattern_Base"), QString()},
    };
    for (const auto &definition : patternNodes) {
        pugi::xml_node node = ensureByTypeAndId(catalog, QStringLiteral("CDataCollectionPattern"), definition.first);
        if (!definition.second.isEmpty())
            setAttribute(node, "parent", definition.second);
        ensureStandardPatternFields(node, definition.first);
    }

    const QHash<QString, QString> parents = standardCollectionParents();
    const QHash<QString, QString> patterns = standardCollectionPatterns();
    const QStringList order{
        QStringLiteral("AbilityMisssile"), QStringLiteral("AbilityBase"),
        QStringLiteral("Weapon_Missile"), QStringLiteral("Weapon_Instant"), QStringLiteral("WeaponBase"),
        QStringLiteral("UnitAir"), QStringLiteral("UnitGround"), QStringLiteral("UnitBase"),
    };
    for (const QString &id : order) {
        pugi::xml_node node = ensureCollectionTemplateById(catalog, id);
        setAttribute(node, "default", QStringLiteral("1"));
        const QString parent = parents.value(id);
        if (!parent.isEmpty())
            setAttribute(node, "parent", parent);
        const QString pattern = patterns.value(id);
        if (!pattern.isEmpty()) {
            pugi::xml_node patternNode = node.child("Pattern");
            if (!patternNode)
                patternNode = node.prepend_child("Pattern");
            setAttribute(patternNode, "value", pattern);
        }
    }
}

void validatePatternInheritance(const AnalysisResult &analysis, const DataNode *collection,
                                const QString &requestedParent, DataCollectionEntityType entity,
                                DataCollectionPreviewReport *report)
{
    QHash<QString, QString> parentByCollection;
    QHash<QString, QString> patternByCollection;
    QSet<QString> patterns;
    for (const DataNode &node : analysis.nodes) {
        if (node.elementName.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive))
            patterns.insert(node.id.toLower());
        else if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)) {
            QString nodeParent;
            QString nodePattern;
            pugi::xml_document fragment;
            if (fragment.load_string(node.serializedXml.toUtf8().constData())) {
                const pugi::xml_node value = fragment.first_child();
                nodeParent = QString::fromUtf8(value.attribute("parent").value());
                nodePattern = QString::fromUtf8(value.child("Pattern").attribute("value").value());
            }
            parentByCollection.insert(node.id.toLower(), nodeParent);
            patternByCollection.insert(node.id.toLower(), nodePattern);
        }
    }
    patterns.unite(standardPatternIds());
    const QHash<QString, QString> standardParents = standardCollectionParents();
    const QHash<QString, QString> standardPatterns = standardCollectionPatterns();
    for (auto it = standardParents.cbegin(); it != standardParents.cend(); ++it)
        if (!parentByCollection.contains(it.key().toLower()))
            parentByCollection.insert(it.key().toLower(), it.value());
    for (auto it = standardPatterns.cbegin(); it != standardPatterns.cend(); ++it)
        if (!patternByCollection.contains(it.key().toLower()))
            patternByCollection.insert(it.key().toLower(), it.value());
    auto fields = [](const DataNode *node, QString *parent, QString *pattern) {
        if (!node) return;
        pugi::xml_document fragment;
        if (!fragment.load_string(node->serializedXml.toUtf8().constData())) return;
        const pugi::xml_node value = fragment.first_child();
        *parent = QString::fromUtf8(value.attribute("parent").value());
        *pattern = QString::fromUtf8(value.child("Pattern").attribute("value").value());
    };

    QString parent;
    fields(collection, &parent, &report->directPattern);
    if (!requestedParent.isEmpty()) parent = requestedParent;
    if (!report->directPattern.isEmpty()) {
        report->effectivePattern = report->directPattern;
        if (!patterns.contains(report->directPattern.toLower())) {
            report->patternState = DataCollectionPatternState::MissingReferencedPattern;
            report->patternDetail = QStringLiteral("Direct Pattern '%1' does not exist.").arg(report->directPattern);
        } else if (!patternMatchesEntity(report->directPattern, entity)) {
            report->patternState = DataCollectionPatternState::InvalidPatternForEntity;
            report->patternDetail = QStringLiteral("Pattern '%1' is incompatible with %2.")
                                        .arg(report->directPattern, dataCollectionEntityTypeName(entity));
        } else {
            report->patternState = DataCollectionPatternState::DirectPattern;
            report->patternDetail = QStringLiteral("Pattern is declared directly.");
        }
        return;
    }

    QSet<QString> visited;
    QString current = parent;
    while (!current.isEmpty()) {
        const QString key = current.toLower();
        if (visited.contains(key)) {
            report->patternState = DataCollectionPatternState::InheritanceCycle;
            report->patternDetail = QStringLiteral("Collection parent chain contains a cycle at '%1'.").arg(current);
            return;
        }
        visited.insert(key);
        if (!parentByCollection.contains(key)) {
            report->patternState = DataCollectionPatternState::MissingParent;
            report->patternDetail = QStringLiteral("Parent collection '%1' does not exist in the analyzed data/dependencies.").arg(current);
            return;
        }
        const QString nextParent = parentByCollection.value(key);
        const QString inherited = patternByCollection.value(key);
        if (!inherited.isEmpty()) {
            report->inheritedPattern = inherited;
            report->effectivePattern = inherited;
            if (!patterns.contains(inherited.toLower())) {
                report->patternState = DataCollectionPatternState::MissingReferencedPattern;
                report->patternDetail = QStringLiteral("Inherited Pattern '%1' does not exist.").arg(inherited);
            } else if (!patternMatchesEntity(inherited, entity)) {
                report->patternState = DataCollectionPatternState::InvalidPatternForEntity;
                report->patternDetail = QStringLiteral("Inherited Pattern '%1' is incompatible with %2.")
                                            .arg(inherited, dataCollectionEntityTypeName(entity));
            } else {
                report->patternState = DataCollectionPatternState::InheritedPattern;
                report->patternDetail = QStringLiteral("Pattern '%1' is inherited through parent '%2'.").arg(inherited, current);
            }
            return;
        }
        current = nextParent;
    }
    report->patternState = DataCollectionPatternState::NoPatternRequired;
    report->patternDetail = QStringLiteral("No Pattern is required; the collection remains valid without specialized field display.");
}

QByteArray addMigrationTargets(const QByteArray &source, const AnalysisResult &analysis,
                               const UnitFamily &sourceFamily, QStringList *moves, QString *error)
{
    if (!sourceFamily.strictOwnership || moves->isEmpty()) return source;
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(source.constData(), size_t(source.size()));
    if (!parsed) {
        *error = QStringLiteral("Cannot stage Data Collection migration: %1").arg(parsed.description());
        return {};
    }
    const QVector<UnitFamily> families = UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
    QHash<QString, UnitFamily> byRoot;
    for (const UnitFamily &family : families) byRoot.insert(family.rootId, family);
    DataCollectionAliasMapper mapper;
    for (QString &move : *moves) {
        const QString entry = move.section(QStringLiteral(" -> "), 0, 0);
        const QString targetId = move.section(QStringLiteral(" -> "), 1).trimmed();
        const auto found = byRoot.constFind(targetId);
        if (found == byRoot.cend()) {
            *error = QStringLiteral("Migration target %1 has no catalog root.").arg(targetId);
            return {};
        }
        const UnitFamily &target = found.value();
        QStringList records;
        for (const UnitFamilyObject &object : target.objects) {
            if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size()
                || object.role == UnitFamilyRole::ManualReview) continue;
            const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex], target.rootId, object.role);
            if (!alias.isEmpty() && !records.contains(alias)) records.append(alias);
        }
        if (!records.contains(entry)) records.prepend(entry);
        sortEntries(&records);
        pugi::xml_node collection = findByTypeAndId(document, target.collectionElementName, target.rootId);
        if (!collection) {
            pugi::xml_node catalog = document.child("Catalog");
            if (!catalog) catalog = document.append_child("Catalog");
            collection = catalog.append_child(target.collectionElementName.toUtf8().constData());
            setAttribute(collection, "id", target.rootId);
        }
        updateCollection(collection, target.recommendedParent,
                         defaultCategoriesFor(analysis.nodes[target.rootNodeIndex]), records, false);
        move = QStringLiteral("%1 -> %2 (create/update, source preserved)").arg(entry, target.rootId);
    }
    std::ostringstream stream;
    document.save(stream, "    ", pugi::format_default, pugi::encoding_utf8);
    return QByteArray::fromStdString(stream.str());
}

QString buildReport(const DataCollectionPreviewReport &preview, const QString &finalResult)
{
    QString report = QStringLiteral("Data Collection Preview\nEntity type: %9\nRoot ID: %1\nRoot XML type: %10\nCollection XML tag: %11\nRequested collection ID: %2\nParent: %3\nEditor categories: %4\nPattern validation: %12\nDirect Pattern: %13\nInherited Pattern: %14\nEffective Pattern: %15\nPattern detail: %16\nTarget file: %5\nArchive entry: %6\nListfile: %7\nListfile update required: %8\n\nReal object IDs / DataRecord entries\n")
                         .arg(preview.request.family.rootId,
                              preview.request.requestedUnitId.isEmpty() ? preview.request.family.rootId : preview.request.requestedUnitId,
                              preview.request.parent, preview.request.editorCategories, preview.targetFile,
                              preview.archiveEntry, preview.listfilePath,
                              preview.listfileNeedsUpdate ? QStringLiteral("yes") : QStringLiteral("no"),
                              dataCollectionEntityTypeName(preview.entityType), preview.rootXmlType,
                              preview.collectionXmlTag, dataCollectionPatternStateName(preview.patternState),
                              preview.directPattern.isEmpty() ? QStringLiteral("none") : preview.directPattern,
                              preview.inheritedPattern.isEmpty() ? QStringLiteral("none") : preview.inheritedPattern,
                              preview.effectivePattern.isEmpty() ? QStringLiteral("none (valid without Pattern)") : preview.effectivePattern,
                              preview.patternDetail);
    for (const DataCollectionEntryProposal &entry : preview.entries)
        report += QStringLiteral("- %1 | %2 | %3 | %4\n").arg(entry.realId, entry.alias, unitFamilyRoleName(entry.role), entry.status);
    const auto section = [&report](const QString &title, const QStringList &values) {
        report += QStringLiteral("\n%1\n").arg(title);
        report += values.isEmpty() ? QStringLiteral("- none\n") : QStringLiteral("- ") + values.join(QStringLiteral("\n- ")) + QLatin1Char('\n');
    };
    section(QStringLiteral("Dynamically discovered objects"), [&]() {
        QStringList values; for (const auto &entry : preview.entries) if (entry.role == UnitFamilyRole::Other
            || entry.role == UnitFamilyRole::Weapon || entry.role == UnitFamilyRole::Ability
            || entry.role == UnitFamilyRole::Effect || entry.role == UnitFamilyRole::Behavior
            || entry.role == UnitFamilyRole::Validator || entry.role == UnitFamilyRole::Requirement
            || entry.role == UnitFamilyRole::Upgrade) values << entry.realId; return values; }());
    section(QStringLiteral("Manual review objects"), preview.manualReviewObjects);
    section(QStringLiteral("Ownership paths"), preview.ownershipPaths);
    section(QStringLiteral("Shared objects"), preview.sharedObjects);
    section(QStringLiteral("Root ID conflicts"), preview.idConflicts);
    section(QStringLiteral("False-positive / unproven legacy associations"), preview.falsePositiveAssociations);
    section(QStringLiteral("Missing expected real objects"), preview.missingExpectedObjects);
    section(QStringLiteral("Existing records preserved"), preview.existingRecordsPreserved);
    section(QStringLiteral("Records to add"), preview.recordsToAdd);
    section(QStringLiteral("Records copied to owner collections"), preview.recordsToMove);
    section(QStringLiteral("Records removed from this collection"), preview.recordsToRemove);
    section(QStringLiteral("Duplicate records skipped"), preview.duplicateRecordsSkipped);
    section(QStringLiteral("Warnings"), preview.warnings);
    report += QStringLiteral("\nGenerated XML\n%1\nFinal result: %2\n").arg(preview.generatedXml, finalResult);
    return report;
}

QString collectionFilePath(const QString &rootFile)
{
    return QDir(QFileInfo(rootFile).absolutePath()).absoluteFilePath(QStringLiteral("DataCollectionData.xml"));
}

QString listfileEntry(const QString &rootFolder, const QString &targetFile)
{
    return QDir(rootFolder).relativeFilePath(targetFile).replace('/', '\\');
}

bool listfileContains(const QString &path, const QString &entry)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString normalizedEntry = QDir::cleanPath(entry).replace('/', '\\');
    for (const QString &line : QString::fromUtf8(file.readAll()).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts))
        if (QDir::cleanPath(line.trimmed()).replace('/', '\\').compare(normalizedEntry, Qt::CaseInsensitive) == 0) return true;
    return false;
}

QByteArray buildCollectionDocument(const QString &targetFile, const QString &elementName, const QString &id, const QString &parent,
                                   const QString &categories, const QStringList &records, bool pruneUnrelated, QString *error)
{
    pugi::xml_document doc;
    QByteArray existingTargetBytes;
    if (QFileInfo::exists(targetFile)) {
        if (!readBytes(targetFile, &existingTargetBytes, error)) return {};
        const auto parsed = doc.load_buffer(existingTargetBytes.constData(), size_t(existingTargetBytes.size()));
        if (!parsed) { *error = QStringLiteral("Cannot parse DataCollectionData.xml: %1").arg(parsed.description()); return {}; }
    } else {
        auto declaration = doc.append_child(pugi::node_declaration);
        declaration.append_attribute("version") = "1.0";
        declaration.append_attribute("encoding") = "utf-8";
        doc.append_child("Catalog");
    }
    pugi::xml_node catalog = doc.child("Catalog");
    if (!catalog) catalog = doc.append_child("Catalog");
    if (!parent.isEmpty())
        ensureStandardDataCollectionSupport(catalog);
    pugi::xml_node collection = findByTypeAndId(doc, elementName, id);
    if (!collection) {
        pugi::xml_node existingTypedDifferently = findCollectionById(doc, id);
        if (existingTypedDifferently)
            collection = retagCollection(catalog, existingTypedDifferently, elementName);
    }
    if (!collection) {
        collection = catalog.append_child(elementName.toUtf8().constData());
        setAttribute(collection, "id", id);
    }
    updateCollection(collection, parent, categories, records, pruneUnrelated);
    std::ostringstream stream;
    doc.save(stream, "    ", pugi::format_default, pugi::encoding_utf8);
    QByteArray staged = QByteArray::fromStdString(stream.str());
    if (!existingTargetBytes.isEmpty()) {
        DataCollectionPreservationReport preservationReport;
        if (!restoreMissingDataCollectionRecords(existingTargetBytes, &staged, &preservationReport, error))
            return {};
    }
    return staged;
}

bool readBytes(const QString &path, QByteArray *bytes, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QStringLiteral("Unable to read %1").arg(path); return false; }
    *bytes = file.readAll(); return true;
}

bool restore(const QString &root, const QString &backup, const QString &relative, QString *error)
{
    const QString target = QDir(root).absoluteFilePath(relative);
    QFile::remove(target);
    if (!QFile::copy(QDir(backup).absoluteFilePath(relative), target)) {
        *error += QStringLiteral(" Rollback failed for %1.").arg(target); return false;
    }
    return true;
}

} // namespace

QString dataCollectionPatternStateName(DataCollectionPatternState state)
{
    switch (state) {
    case DataCollectionPatternState::DirectPattern: return QStringLiteral("Direct Pattern");
    case DataCollectionPatternState::InheritedPattern: return QStringLiteral("Inherited Pattern");
    case DataCollectionPatternState::NoPatternRequired: return QStringLiteral("No Pattern Required");
    case DataCollectionPatternState::MissingParent: return QStringLiteral("Missing Parent");
    case DataCollectionPatternState::MissingReferencedPattern: return QStringLiteral("Missing Referenced Pattern");
    case DataCollectionPatternState::InvalidPatternForEntity: return QStringLiteral("Invalid Pattern For Entity");
    case DataCollectionPatternState::InheritanceCycle: return QStringLiteral("Inheritance Cycle");
    }
    return QStringLiteral("No Pattern Required");
}

DataCollectionAuditSummary auditDataCollections(const AnalysisResult &analysis)
{
    DataCollectionAuditSummary summary;
    QHash<QString, QSet<int>> rootTypes;
    for (const DataNode &node : analysis.nodes) {
        if (node.id.isEmpty()) continue;
        if (node.elementName.compare(QStringLiteral("CUnit"), Qt::CaseInsensitive) == 0)
            rootTypes[node.id.toLower()].insert(int(DataCollectionEntityType::Unit));
        else if (node.elementName.startsWith(QStringLiteral("CAbil"), Qt::CaseInsensitive))
            rootTypes[node.id.toLower()].insert(int(DataCollectionEntityType::Ability));
        else if (node.elementName.startsWith(QStringLiteral("CWeapon"), Qt::CaseInsensitive))
            rootTypes[node.id.toLower()].insert(int(DataCollectionEntityType::Weapon));
    }
    for (const DataNode &node : analysis.nodes) {
        if (!node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
            || node.elementName.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive)) continue;
        pugi::xml_document fragment;
        if (!fragment.load_string(node.serializedXml.toUtf8().constData())) continue;
        const pugi::xml_node collection = fragment.first_child();
        if (QString::fromUtf8(collection.attribute("default").value()) == QStringLiteral("1")) continue;
        ++summary.collections;
        const QSet<int> types = rootTypes.value(node.id.toLower());
        if (types.isEmpty()) {
            ++summary.unanchoredCollections;
            summary.manualReview << QStringLiteral("%1: Unanchored (no CUnit/CAbil*/CWeapon* root)").arg(node.id);
            continue;
        }
        if (types.size() > 1) {
            ++summary.rootTypeConflicts;
            summary.manualReview << QStringLiteral("%1: Root Type Conflict").arg(node.id);
            continue;
        }
        const DataCollectionEntityType entity = DataCollectionEntityType(*types.cbegin());
        if (entity == DataCollectionEntityType::Unit) ++summary.unitCollections;
        else if (entity == DataCollectionEntityType::Ability) ++summary.abilityCollections;
        else ++summary.weaponCollections;

        QSet<QString> primaryKinds;
        bool primaryFound = false;
        const QString expectedCatalog = entity == DataCollectionEntityType::Unit ? QStringLiteral("Unit")
            : entity == DataCollectionEntityType::Ability ? QStringLiteral("Abil") : QStringLiteral("Weapon");
        for (pugi::xml_node record : collection.children("DataRecord")) {
            const QString entry = QString::fromUtf8(record.attribute("Entry").value());
            const QString catalog = entry.section(QLatin1Char(','), 0, 0).trimmed();
            const QString id = entry.section(QLatin1Char(','), 1).trimmed();
            if (catalog == QStringLiteral("Unit") || catalog == QStringLiteral("Abil") || catalog == QStringLiteral("Weapon"))
                primaryKinds.insert(catalog);
            if (catalog.compare(expectedCatalog, Qt::CaseInsensitive) == 0 && id == node.id) primaryFound = true;
        }
        if (primaryKinds.size() > 1) ++summary.mixedRootCollections;
        if (!primaryFound) {
            ++summary.missingPrimaryRecords;
            summary.manualReview << QStringLiteral("%1: missing %2,%1").arg(node.id, expectedCatalog);
        }
        const QString categories = QString::fromUtf8(collection.child("EditorCategories").attribute("value").value());
        const QString expectedGroup = entity == DataCollectionEntityType::Unit ? QStringLiteral("DataGroup:Unit")
            : entity == DataCollectionEntityType::Ability ? QStringLiteral("DataGroup:Ability") : QStringLiteral("DataGroup:Weapon");
        if (!categories.contains(expectedGroup, Qt::CaseInsensitive)
            || (entity != DataCollectionEntityType::Unit && categories.contains(QStringLiteral("ObjectType:Unit"), Qt::CaseInsensitive)))
            ++summary.invalidCategories;

        DataCollectionPreviewReport pattern;
        validatePatternInheritance(analysis, &node, {}, entity, &pattern);
        if (pattern.patternState == DataCollectionPatternState::DirectPattern) ++summary.directPatterns;
        else if (pattern.patternState == DataCollectionPatternState::InheritedPattern) ++summary.inheritedPatterns;
        else if (pattern.patternState == DataCollectionPatternState::NoPatternRequired) ++summary.validWithoutPattern;
        else if (pattern.patternState == DataCollectionPatternState::MissingParent) {
            ++summary.missingParents; ++summary.brokenInheritance;
        } else ++summary.brokenInheritance;
    }
    summary.reportText = QStringLiteral(
        "Collections: %1 | Unit: %2 | Ability: %3 | Weapon: %4 | Unanchored: %5 | Mixed-root: %6 | "
        "Root conflicts: %7 | Missing primary: %8 | Invalid categories: %9 | Direct Pattern: %10 | "
        "Inherited Pattern: %11 | Valid without Pattern: %12 | Missing parent: %13 | Broken inheritance: %14")
        .arg(summary.collections).arg(summary.unitCollections).arg(summary.abilityCollections).arg(summary.weaponCollections)
        .arg(summary.unanchoredCollections).arg(summary.mixedRootCollections).arg(summary.rootTypeConflicts)
        .arg(summary.missingPrimaryRecords).arg(summary.invalidCategories).arg(summary.directPatterns)
        .arg(summary.inheritedPatterns).arg(summary.validWithoutPattern).arg(summary.missingParents).arg(summary.brokenInheritance);
    return summary;
}

DataCollectionPreviewReport DataCollectionUnitBuilder::preview(const AnalysisResult &analysis,
                                                               const DataCollectionBuildRequest &request,
                                                               const QVector<UnitFamily> *knownFamilies) const
{
    DataCollectionPreviewReport result;
    result.request = request;
    if (request.family.rootNodeIndex < 0 || request.family.rootNodeIndex >= analysis.nodes.size()) {
        result.warnings << QStringLiteral("Select a unit family."); result.reportText = buildReport(result, QStringLiteral("blocked")); return result;
    }
    const QString root = request.family.rootId;
    const QString requestedCollectionId = request.requestedUnitId.trimmed().isEmpty() ? root : request.requestedUnitId.trimmed();
    const bool nameMatchesCollection = requestedCollectionId == root;
    if (!nameMatchesCollection)
        result.warnings << QStringLiteral("The requested Collection ID differs from the detected CollectionID@ child prefix. Rename the real XML IDs first.");
    const DataNode &rootNode = analysis.nodes[request.family.rootNodeIndex];
    result.entityType = request.family.entityType;
    result.rootXmlType = rootNode.elementName;
    result.collectionXmlTag = request.family.collectionElementName;
    for (const UnitFamilyObject &object : request.family.objects) {
        if (object.nodeIndex >= 0 && object.nodeIndex < analysis.nodes.size() && object.nodeIndex != request.family.rootNodeIndex)
            result.ownershipPaths << QStringLiteral("%1: %2").arg(analysis.nodes[object.nodeIndex].id, object.reason);
    }
    if (request.family.rootTypeConflict) {
        result.idConflicts << QStringLiteral("Root ID '%1' exists in multiple primary catalogs; automatic Apply is blocked.").arg(root);
        result.warnings << result.idConflicts;
    }
    const DataNode *existingCollectionNode = nullptr;
    for (const DataNode &node : analysis.nodes)
        if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
            && !node.elementName.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive) && node.id == root)
            existingCollectionNode = &node;
    result.existingCollection = existingCollectionNode != nullptr;
    result.targetFile = existingCollectionNode ? existingCollectionNode->sourceFile : collectionFilePath(rootNode.sourceFile);
    result.targetFileExists = QFileInfo::exists(result.targetFile);
    result.listfilePath = QDir(analysis.rootFolder).absoluteFilePath(QStringLiteral("(listfile)"));
    result.archiveEntry = listfileEntry(analysis.rootFolder, result.targetFile);
    result.listfileNeedsUpdate = !listfileContains(result.listfilePath, result.archiveEntry);

    DataCollectionAliasMapper mapper;
    QHash<QString, QSet<QString>> ownersByAlias;
    QHash<QString, QString> canonicalOwnerByAlias;
    if (request.family.strictOwnership) {
        const auto buildOwnershipMaps = [&](const QVector<UnitFamily> &typedFamilies,
                                            QHash<QString, QSet<QString>> *owners,
                                            QHash<QString, QString> *canonicalOwners) {
            owners->clear();
            canonicalOwners->clear();
            QHash<QString, QVector<AliasOwnerCandidate>> ownerCandidatesByAlias;
            for (const UnitFamily &family : typedFamilies) {
                for (const UnitFamilyObject &object : family.objects) {
                    if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size()
                        || (object.role == UnitFamilyRole::ManualReview && !isSharedManualObject(object))) continue;
                    const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex], family.rootId, object.role);
                    if (alias.isEmpty()) continue;
                    const QString aliasKey = alias.toLower();
                    (*owners)[aliasKey].insert(family.rootId);
                    ownerCandidatesByAlias[aliasKey].append({
                        family.rootId,
                        family.entityType,
                        idIsScopedToRoot(analysis.nodes[object.nodeIndex].id, family.rootId),
                        ownershipPathDepth(object.reason),
                    });
                }
            }
            for (auto it = ownerCandidatesByAlias.cbegin(); it != ownerCandidatesByAlias.cend(); ++it) {
                QVector<AliasOwnerCandidate> candidates = it.value();
                std::sort(candidates.begin(), candidates.end(), [](const AliasOwnerCandidate &left, const AliasOwnerCandidate &right) {
                    if (left.scoped != right.scoped) return left.scoped && !right.scoped;
                    if (left.pathDepth != right.pathDepth) return left.pathDepth < right.pathDepth;
                    const int leftPriority = entityPriority(left.entityType), rightPriority = entityPriority(right.entityType);
                    if (leftPriority != rightPriority) return leftPriority < rightPriority;
                    return left.rootId.compare(right.rootId, Qt::CaseInsensitive) < 0;
                });
                if (!candidates.isEmpty())
                    canonicalOwners->insert(it.key(), candidates.front().rootId);
            }
        };
        if (knownFamilies && request.summaryOnly) {
            if (m_cachedOwnerFamilies != knownFamilies) {
                buildOwnershipMaps(*knownFamilies, &m_cachedOwnersByAlias, &m_cachedCanonicalOwnerByAlias);
                m_cachedOwnerFamilies = knownFamilies;
            }
            ownersByAlias = m_cachedOwnersByAlias;
            canonicalOwnerByAlias = m_cachedCanonicalOwnerByAlias;
        } else {
            const QVector<UnitFamily> detectedFamilies = knownFamilies ? QVector<UnitFamily>()
                : UnitFamilyDetector().detectCollectionFamilies(analysis, DataCollectionMode::UnitAbilWeapon);
            const QVector<UnitFamily> &typedFamilies = knownFamilies ? *knownFamilies : detectedFamilies;
            buildOwnershipMaps(typedFamilies, &ownersByAlias, &canonicalOwnerByAlias);
        }
    }
    QSet<QString> desiredAliases;
    QHash<QString, QString> desiredAliasByKey;
    for (const UnitFamilyObject &object : request.family.objects) {
        if (object.nodeIndex < 0 || object.nodeIndex >= analysis.nodes.size())
            continue;
        const QString alias = mapper.aliasFor(analysis.nodes[object.nodeIndex], root, object.role);
        if (alias.isEmpty())
            continue;
        const QString aliasKey = alias.toLower();
        if (object.role == UnitFamilyRole::ManualReview && !request.confirmNonStandard)
            continue;
        desiredAliases.insert(aliasKey);
        desiredAliasByKey.insert(aliasKey, alias);
    }
    QSet<QString> knownAliases;
    QStringList existing;
    QSet<QString> migrationTargets;
    if (existingCollectionNode) {
        pugi::xml_document fragment;
        if (fragment.load_string(existingCollectionNode->serializedXml.toUtf8().constData())) {
            const QStringList allExisting = existingEntries(fragment.first_child(), &result.duplicateRecordsSkipped);
            for (const QString &entry : allExisting) {
                const QString entryKey = entry.toLower();
                const QSet<QString> owners = ownersByAlias.value(entryKey);
                if (owners.size() > 1) {
                    const QString canonicalOwner = canonicalOwnerByAlias.value(entryKey);
                    result.sharedObjects << QStringLiteral("%1 <- %2 | owner: %3")
                                                .arg(entry,
                                                     QStringList(owners.cbegin(), owners.cend()).join(QStringLiteral(", ")),
                                                     canonicalOwner.isEmpty() ? QStringLiteral("manual") : canonicalOwner);
                    result.manualReviewObjects << entry;
                    existing << entry;
                    if (!canonicalOwner.isEmpty() && canonicalOwner.compare(root, Qt::CaseInsensitive) != 0
                        && !migrationTargets.contains(canonicalOwner)) {
                        result.recordsToMove << QStringLiteral("%1 -> %2").arg(entry, canonicalOwner);
                        migrationTargets.insert(canonicalOwner);
                    }
                    continue;
                }
                const QString owner = owners.isEmpty() ? QString() : *owners.cbegin();
                if (request.family.strictOwnership && !desiredAliases.contains(entry.toLower())
                    && !owner.isEmpty() && owner.compare(root, Qt::CaseInsensitive) != 0) {
                    if (!migrationTargets.contains(owner)) {
                        result.recordsToMove << QStringLiteral("%1 -> %2").arg(entry, owner);
                        migrationTargets.insert(owner);
                    }
                    existing << entry;
                } else {
                    if (request.family.strictOwnership && !desiredAliases.contains(entry.toLower()) && owner.isEmpty()) {
                        result.falsePositiveAssociations << entry;
                        result.manualReviewObjects << entry;
                    }
                    existing << entry;
                }
            }
            result.existingRecordsPreserved = existing;
            knownAliases = QSet<QString>(existing.cbegin(), existing.cend());
        }
    }
    QSet<QString> knownAliasKeys;
    for (const QString &entry : existing)
        knownAliasKeys.insert(entry.toLower());
    QSet<int> included = request.includedNodeIndices;
    if (included.isEmpty()) for (const UnitFamilyObject &object : request.family.objects)
        if (object.role != UnitFamilyRole::ManualReview || request.confirmNonStandard) included.insert(object.nodeIndex);
    included.insert(request.family.rootNodeIndex); // the primary DataRecord is mandatory
    QHash<int, UnitFamilyObject> familyObjects;
    for (const UnitFamilyObject &object : request.family.objects) familyObjects.insert(object.nodeIndex, object);
    bool standardized = true;
    int linkedObjectCount = 0;
    for (const UnitFamilyObject &object : request.family.objects) {
        const DataNode &node = analysis.nodes[object.nodeIndex];
        DataCollectionEntryProposal proposal;
        proposal.nodeIndex = object.nodeIndex; proposal.realType = node.elementName; proposal.realId = node.id;
        proposal.role = object.role; proposal.confidence = object.confidence; proposal.included = included.contains(object.nodeIndex);
        const bool validCollectionId = node.id.compare(root, Qt::CaseInsensitive) == 0
            || node.id.startsWith(root + QLatin1Char('@'), Qt::CaseInsensitive);
        proposal.alias = mapper.aliasFor(node, root, object.role);
        if (proposal.alias.isEmpty()) {
            proposal.status = QStringLiteral("Manual Review: unsupported catalog type");
            result.manualReviewObjects << node.id;
            standardized = false;
        } else if (object.role == UnitFamilyRole::ManualReview) {
            result.manualReviewObjects << node.id;
            if (isSharedManualObject(object))
                result.sharedObjects << QStringLiteral("%1 (%2)").arg(proposal.alias, object.reason);
            if (proposal.included && request.confirmNonStandard) {
                if (knownAliases.contains(proposal.alias)) proposal.status = QStringLiteral("Already exists (manually confirmed)");
                else { proposal.status = QStringLiteral("Will add (manually confirmed)"); result.recordsToAdd << proposal.alias; ++linkedObjectCount; }
            } else proposal.status = QStringLiteral("Manual Review");
        } else if (knownAliases.contains(proposal.alias)) {
            proposal.status = validCollectionId ? QStringLiteral("Already exists")
                                                : QStringLiteral("Already exists (non-standard ID)");
            ++linkedObjectCount;
        } else if (proposal.included) {
            proposal.status = validCollectionId ? QStringLiteral("Will add")
                                                : QStringLiteral("Will add (non-standard ID)");
            result.recordsToAdd << proposal.alias;
            ++linkedObjectCount;
        }
        else proposal.status = QStringLiteral("Excluded");
        result.entries << proposal;
        if (!validCollectionId)
            standardized = false;
    }
    result.familyStandardized = standardized;
    if (!standardized)
        result.warnings << QStringLiteral("Family contains existing real IDs outside CollectionID / CollectionID@Child format. They can still be linked, but naming is non-standard.");
    if (!result.existingCollection && linkedObjectCount < 1)
        result.warnings << QStringLiteral("A new collection requires at least one existing catalog object.");
    if (!result.falsePositiveAssociations.isEmpty())
        result.warnings << QStringLiteral("Automatic Apply is blocked: legacy DataRecord entries without a proven entity owner require Manual Review.");

    QString existingParent;
    if (existingCollectionNode) {
        pugi::xml_document fragment;
        if (fragment.load_string(existingCollectionNode->serializedXml.toUtf8().constData()))
            existingParent = QString::fromUtf8(fragment.first_child().attribute("parent").value());
    }
    const QString effectiveParent = !request.parent.trimmed().isEmpty() ? request.parent.trimmed()
        : !existingParent.isEmpty() ? existingParent : request.family.recommendedParent;
    validatePatternInheritance(analysis, existingCollectionNode, effectiveParent, request.family.entityType, &result);
    const bool brokenInheritance = result.patternState == DataCollectionPatternState::MissingParent
        || result.patternState == DataCollectionPatternState::MissingReferencedPattern
        || result.patternState == DataCollectionPatternState::InvalidPatternForEntity
        || result.patternState == DataCollectionPatternState::InheritanceCycle;
    if (brokenInheritance) result.warnings << result.patternDetail;

    if (request.summaryOnly) {
        for (const QString &aliasKey : desiredAliases) {
            if (!knownAliasKeys.contains(aliasKey))
                result.recordsToAdd << desiredAliasByKey.value(aliasKey);
        }
        sortEntries(&result.recordsToAdd);
        const int linkedObjectCount = result.existingRecordsPreserved.size() + result.recordsToAdd.size();
        result.valid = nameMatchesCollection && !request.family.rootTypeConflict && !brokenInheritance
            && result.falsePositiveAssociations.isEmpty()
            && (result.existingCollection || linkedObjectCount >= 1);
        result.reportText = buildReport(result, QStringLiteral("Summary preview; XML generation deferred until apply"));
        return result;
    }

    QStringList allRecords = existing;
    for (const QString &entry : result.recordsToAdd) if (!allRecords.contains(entry)) allRecords << entry;
    sortEntries(&allRecords);
    QString generatedError;
    const QString elementName = request.family.strictOwnership
        ? request.family.collectionElementName
        : existingCollectionNode ? existingCollectionNode->elementName : request.family.collectionElementName;
    const QString parent = effectiveParent;
    QString categories = request.editorCategories.trimmed();
    if (categories.isEmpty() && existingCollectionNode) {
        pugi::xml_document fragment;
        if (fragment.load_string(existingCollectionNode->serializedXml.toUtf8().constData()))
            categories = QString::fromUtf8(fragment.first_child().child("EditorCategories").attribute("value").value());
    }
    if (categories.isEmpty()) categories = defaultCategoriesFor(rootNode);
    const QString expectedGroup = request.family.entityType == DataCollectionEntityType::Unit ? QStringLiteral("DataGroup:Unit")
        : request.family.entityType == DataCollectionEntityType::Ability ? QStringLiteral("DataGroup:Ability")
                                                                        : QStringLiteral("DataGroup:Weapon");
    if (!categories.contains(expectedGroup, Qt::CaseInsensitive)
        || (request.family.entityType != DataCollectionEntityType::Unit
            && categories.contains(QStringLiteral("ObjectType:Unit"), Qt::CaseInsensitive))) {
        const QString invalid = categories;
        if (request.family.strictOwnership) {
            categories = defaultCategoriesFor(rootNode);
            result.warnings << QStringLiteral("EditorCategories '%1' contradicted the %2 root and were normalized to '%3'.")
                                   .arg(invalid, dataCollectionEntityTypeName(request.family.entityType), categories);
        } else {
            generatedError = QStringLiteral("EditorCategories '%1' contradict the %2 root.")
                                 .arg(categories, dataCollectionEntityTypeName(request.family.entityType));
        }
    }
    QByteArray generated;
    if (generatedError.isEmpty())
        generated = buildCollectionDocument(result.targetFile, elementName, root, parent,
                                            categories, allRecords,
                                            request.family.strictOwnership, &generatedError);
    for (const QString &move : result.recordsToMove) {
        const QString targetId = move.section(QStringLiteral(" -> "), 1).trimmed();
        for (const DataNode &node : analysis.nodes) {
            if (node.elementName.startsWith(QStringLiteral("CDataCollection"), Qt::CaseInsensitive)
                && !node.elementName.startsWith(QStringLiteral("CDataCollectionPattern"), Qt::CaseInsensitive)
                && node.id == targetId && QFileInfo(node.sourceFile).absoluteFilePath() != QFileInfo(result.targetFile).absoluteFilePath()) {
                generatedError = QStringLiteral("Atomic migration blocked: target collection %1 is stored in another XML file (%2).")
                                     .arg(targetId, node.sourceFile);
                break;
            }
        }
        if (!generatedError.isEmpty()) break;
    }
    if (generatedError.isEmpty() && !result.recordsToMove.isEmpty())
        generated = addMigrationTargets(generated, analysis, request.family, &result.recordsToMove, &generatedError);
    result.generatedXml = QString::fromUtf8(generated);
    if (!generatedError.isEmpty()) result.warnings << generatedError;
    result.valid = nameMatchesCollection && !request.family.rootTypeConflict && !brokenInheritance
        && result.falsePositiveAssociations.isEmpty() && generatedError.isEmpty()
        && (result.existingCollection || linkedObjectCount >= 1);
    result.reportText = buildReport(result, QStringLiteral("Preview only; no files modified"));
    return result;
}

DataCollectionApplyResult DataCollectionUnitBuilder::apply(const AnalysisResult &analysis,
                                                           const DataCollectionBuildRequest &request,
                                                           const QString &rootFolder,
                                                           const QSet<QString> &whitelistIds,
                                                           bool verifyWithFullReanalysis,
                                                           const QVector<UnitFamily> *knownFamilies,
                                                           bool transientWorkspace) const
{
    DataCollectionApplyResult result;
    const DataCollectionPreviewReport plan = preview(analysis, request, knownFamilies);
    if (!plan.valid) { result.error = plan.warnings.join(QStringLiteral("; ")); return result; }
    QByteArray staged = plan.generatedXml.toUtf8();
    const QString relative = QDir(rootFolder).relativeFilePath(plan.targetFile);
    const QString listRelative = QDir(rootFolder).relativeFilePath(plan.listfilePath);
    const bool targetExisted = QFileInfo::exists(plan.targetFile);
    const bool listfileExisted = QFileInfo::exists(plan.listfilePath);
    if (targetExisted) {
        QByteArray existingTargetBytes;
        if (!readBytes(plan.targetFile, &existingTargetBytes, &result.error)) return result;
        DataCollectionPreservationReport preservationReport;
        if (!restoreMissingDataCollectionRecords(existingTargetBytes, &staged, &preservationReport, &result.error)) return result;
    }
    QByteArray listfileBytes;
    if (listfileExisted && !readBytes(plan.listfilePath, &listfileBytes, &result.error)) return result;
    if (plan.listfileNeedsUpdate) {
        if (!listfileBytes.isEmpty() && !listfileBytes.endsWith('\n')) listfileBytes.append("\r\n");
        listfileBytes.append(plan.archiveEntry.toUtf8());
        listfileBytes.append("\r\n");
    }
    QStringList existingForBackup;
    if (targetExisted) existingForBackup << relative;
    if (listfileExisted) existingForBackup << listRelative;
    BackupManager backup;
    if (!transientWorkspace
        && !backup.createFolderBackup(rootFolder, existingForBackup, analysis.analysisReportText, plan.reportText,
                                      &result.backupFolder, &result.error)) return result;
    if (m_failureInjectionStep == QStringLiteral("after-backup")) { result.error = QStringLiteral("Injected failure after backup."); return result; }
    const auto rollback = [&]() {
        if (transientWorkspace) return;
        if (result.backupFolder.startsWith(QStringLiteral("disabled"), Qt::CaseInsensitive)) return;
        if (targetExisted) restore(rootFolder, result.backupFolder, relative, &result.error); else QFile::remove(plan.targetFile);
        if (listfileExisted) restore(rootFolder, result.backupFolder, listRelative, &result.error); else QFile::remove(plan.listfilePath);
    };
    QDir().mkpath(QFileInfo(plan.targetFile).absolutePath());
    QSaveFile output(plan.targetFile);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || output.write(staged) != staged.size() || !output.commit()) {
        result.error = QStringLiteral("Unable to commit Data Collection XML."); return result;
    }
    if (plan.listfileNeedsUpdate || !listfileExisted) {
        QSaveFile listfile(plan.listfilePath);
        if (!listfile.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || listfile.write(listfileBytes) != listfileBytes.size() || !listfile.commit()) {
            result.error = QStringLiteral("Unable to update (listfile)."); rollback(); return result;
        }
    }
    if (m_failureInjectionStep == QStringLiteral("after-commit")) {
        result.error = QStringLiteral("Injected failure after commit."); rollback(); return result;
    }
    Q_UNUSED(whitelistIds);
    QByteArray verifiedBytes;
    QString verifyError;
    if (!readBytes(plan.targetFile, &verifiedBytes, &verifyError)) {
        result.error = QStringLiteral("Collection verification failed: %1").arg(verifyError); rollback(); return result;
    }
    pugi::xml_document verifiedDocument;
    const auto parsed = verifiedDocument.load_buffer(verifiedBytes.constData(), size_t(verifiedBytes.size()));
    if (!parsed) {
        result.error = QStringLiteral("Collection verification failed: %1").arg(parsed.description()); rollback(); return result;
    }
    const pugi::xml_node verified = findByTypeAndId(verifiedDocument, plan.collectionXmlTag, request.family.rootId);
    if (!verified) { result.error = QStringLiteral("Collection verification failed: object missing."); rollback(); return result; }
    QStringList verifyDuplicates; const QStringList verifiedEntries = existingEntries(verified, &verifyDuplicates);
    for (const QString &entry : plan.recordsToAdd) if (!verifiedEntries.contains(entry)) {
        result.error = QStringLiteral("Collection verification failed: %1 missing.").arg(entry); rollback(); return result;
    }
    for (const QString &entry : plan.recordsToRemove) if (verifiedEntries.contains(entry)) {
        result.error = QStringLiteral("Collection verification failed: unrelated record %1 remains.").arg(entry); rollback(); return result;
    }
    for (const QString &move : plan.recordsToMove) {
        const QString entry = move.section(QStringLiteral(" -> "), 0, 0);
        const QString targetId = move.section(QStringLiteral(" -> "), 1).section(QStringLiteral(" ("), 0, 0).trimmed();
        const pugi::xml_node target = findCollectionById(verifiedDocument, targetId);
        if (!target || !existingEntries(target).contains(entry)) {
            result.error = QStringLiteral("Collection verification failed: migration target %1 does not contain %2.").arg(targetId, entry);
            rollback(); return result;
        }
    }
    Q_UNUSED(verifyDuplicates);
    if (!listfileContains(plan.listfilePath, plan.archiveEntry)) {
        result.error = QStringLiteral("Collection verification failed: (listfile) entry is missing."); rollback(); return result;
    }
    if (verifyWithFullReanalysis) {
        AnalysisResult reanalyzed;
        QString reanalysisError;
        if (!FolderAnalyzer().analyzeFolder(rootFolder, whitelistIds, &reanalyzed, &reanalysisError)) {
            result.error = QStringLiteral("Post-apply analysis failed: %1").arg(reanalysisError); rollback(); return result;
        }
    }
    result.success = true; result.changedFile = relative; result.recordsAdded = plan.recordsToAdd.size();
    result.recordsRemoved = plan.recordsToRemove.size();
    result.changedFiles << relative;
    if (plan.listfileNeedsUpdate || !listfileExisted) result.changedFiles << listRelative;
    result.duplicatesSkipped = plan.duplicateRecordsSkipped.size();
    result.finalReport = plan.reportText + (transientWorkspace
        ? QStringLiteral("\nFinal result after apply: success in temporary archive workspace\n")
        : QStringLiteral("\nFinal result after apply: success\nBackup: %1\n").arg(result.backupFolder));
    return result;
}
