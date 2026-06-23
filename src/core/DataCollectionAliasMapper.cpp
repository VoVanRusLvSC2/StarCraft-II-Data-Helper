#include "core/DataCollectionAliasMapper.h"

QString DataCollectionAliasMapper::catalogType(const QString &elementName) const
{
    const QString type = elementName.toLower();
    if (type == QStringLiteral("cunit")) return QStringLiteral("Unit");
    if (type.startsWith(QStringLiteral("cactor"))) return QStringLiteral("Actor");
    if (type.startsWith(QStringLiteral("cbutton"))) return QStringLiteral("Button");
    if (type.startsWith(QStringLiteral("cmodel"))) return QStringLiteral("Model");
    if (type.startsWith(QStringLiteral("ctexture"))) return QStringLiteral("Texture");
    if (type.startsWith(QStringLiteral("cterraintex"))) return QStringLiteral("TerrainTex");
    if (type.startsWith(QStringLiteral("csoundtrack"))) return QStringLiteral("Soundtrack");
    if (type.startsWith(QStringLiteral("csound"))) return QStringLiteral("Sound");
    if (type.startsWith(QStringLiteral("cweapon"))) return QStringLiteral("Weapon");
    if (type.startsWith(QStringLiteral("cabil"))) return QStringLiteral("Abil");
    if (type.startsWith(QStringLiteral("ceffect"))) return QStringLiteral("Effect");
    if (type.startsWith(QStringLiteral("cbehavior"))) return QStringLiteral("Behavior");
    if (type.startsWith(QStringLiteral("cvalidator"))) return QStringLiteral("Validator");
    if (type.startsWith(QStringLiteral("crequirementnode"))) return QStringLiteral("RequirementNode");
    if (type.startsWith(QStringLiteral("crequirement"))) return QStringLiteral("Requirement");
    if (type.startsWith(QStringLiteral("cupgrade"))) return QStringLiteral("Upgrade");
    if (type.startsWith(QStringLiteral("crace"))) return QStringLiteral("Race");
    if (type.startsWith(QStringLiteral("creward"))) return QStringLiteral("Reward");
    if (type.startsWith(QStringLiteral("cspray"))) return QStringLiteral("Spray");
    if (type.startsWith(QStringLiteral("ccommander"))) return QStringLiteral("Commander");
    if (type.startsWith(QStringLiteral("cturret"))) return QStringLiteral("Turret");
    if (type.startsWith(QStringLiteral("cmover"))) return QStringLiteral("Mover");
    if (type.startsWith(QStringLiteral("cfootprint"))) return QStringLiteral("Footprint");
    if (type.startsWith(QStringLiteral("csiteop"))) return QStringLiteral("SiteOp");
    if (type.startsWith(QStringLiteral("cbeam"))) return QStringLiteral("Beam");
    return {};
}

QString DataCollectionAliasMapper::aliasFor(const DataNode &node, const QString &rootId, UnitFamilyRole role) const
{
    Q_UNUSED(rootId);
    Q_UNUSED(role);
    const QString catalog = catalogType(node.elementName);
    if (catalog.isEmpty() || node.id.trimmed().isEmpty()) return {};
    // DataRecord stores the real catalog ID verbatim. '@' is not an alias
    // operator invented by the collection: it is part of the actual SC2 ID
    // when the corresponding catalog object was created as a collection child.
    return QStringLiteral("%1,%2").arg(catalog, node.id);
}
