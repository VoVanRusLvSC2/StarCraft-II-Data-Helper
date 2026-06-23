#include "core/DataCollectionAliasMapper.h"

QString DataCollectionAliasMapper::catalogType(const QString &elementName) const
{
    const QString type = elementName.toLower();
    if (type == QStringLiteral("cunit")) return QStringLiteral("Unit");
    if (type.startsWith(QStringLiteral("cactor"))) return QStringLiteral("Actor");
    if (type.startsWith(QStringLiteral("cbutton"))) return QStringLiteral("Button");
    if (type.startsWith(QStringLiteral("cmodel"))) return QStringLiteral("Model");
    if (type.startsWith(QStringLiteral("csound"))) return QStringLiteral("Sound");
    if (type.startsWith(QStringLiteral("cweapon"))) return QStringLiteral("Weapon");
    if (type.startsWith(QStringLiteral("cabil"))) return QStringLiteral("Abil");
    if (type.startsWith(QStringLiteral("ceffect"))) return QStringLiteral("Effect");
    if (type.startsWith(QStringLiteral("cbehavior"))) return QStringLiteral("Behavior");
    if (type.startsWith(QStringLiteral("cvalidator"))) return QStringLiteral("Validator");
    if (type.startsWith(QStringLiteral("crequirement"))) return QStringLiteral("Requirement");
    if (type.startsWith(QStringLiteral("cupgrade"))) return QStringLiteral("Upgrade");
    return {};
}

QString DataCollectionAliasMapper::aliasFor(const DataNode &node, const QString &rootId, UnitFamilyRole role) const
{
    const QString catalog = catalogType(node.elementName);
    if (catalog.isEmpty() || node.id.contains(QLatin1Char('@'))) return {};
    if (role == UnitFamilyRole::Unit && node.id == rootId) return QStringLiteral("Unit,%1").arg(rootId);
    if (!node.id.startsWith(rootId, Qt::CaseInsensitive) || node.id.size() <= rootId.size()) return {};
    const QString suffix = node.id.mid(rootId.size());
    return QStringLiteral("%1,%2@%3").arg(catalog, rootId, suffix);
}
