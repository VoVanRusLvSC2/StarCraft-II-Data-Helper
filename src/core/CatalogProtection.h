#pragma once

#include "core/DataNode.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace sc2dh
{

inline QString catalogTokenKey(const QString &token)
{
    return token.trimmed().toCaseFolded();
}

inline bool isReservedCatalogToken(const QString &token)
{
    const QString key = catalogTokenKey(token);
    if (key.isEmpty())
        return false;

    static const QSet<QString> tokens = [] {
        QSet<QString> values = {
            QStringLiteral("air"),
            QStringLiteral("ally"),
            QStringLiteral("alive"),
            QStringLiteral("armored"),
            QStringLiteral("astimescale"),
            QStringLiteral("asduration"),
            QStringLiteral("attack"),
            QStringLiteral("actorcreation"),
            QStringLiteral("animbracketstart"),
            QStringLiteral("animbracketstop"),
            QStringLiteral("animclear"),
            QStringLiteral("animplay"),
            QStringLiteral("beam1"),
            QStringLiteral("beam2"),
            QStringLiteral("benign"),
            QStringLiteral("biological"),
            QStringLiteral("birth"),
            QStringLiteral("bsd"),
            QStringLiteral("buried"),
            QStringLiteral("cast"),
            QStringLiteral("channel"),
            QStringLiteral("cloaked"),
            QStringLiteral("contentloop"),
            QStringLiteral("contentplayonce"),
            QStringLiteral("cover"),
            QStringLiteral("dead"),
            QStringLiteral("default"),
            QStringLiteral("destructible"),
            QStringLiteral("detected"),
            QStringLiteral("death"),
            QStringLiteral("enemy"),
            QStringLiteral("energy"),
            QStringLiteral("excluded"),
            QStringLiteral("fast"),
            QStringLiteral("ground"),
            QStringLiteral("hallucination"),
            QStringLiteral("harvestableresource"),
            QStringLiteral("hero"),
            QStringLiteral("heroic"),
            QStringLiteral("hidden"),
            QStringLiteral("host"),
            QStringLiteral("hostimpact"),
            QStringLiteral("hostlaunch"),
            QStringLiteral("hover"),
            QStringLiteral("invulnerable"),
            QStringLiteral("item"),
            QStringLiteral("light"),
            QStringLiteral("life"),
            QStringLiteral("massive"),
            QStringLiteral("mechanical"),
            QStringLiteral("missile"),
            QStringLiteral("modelswap"),
            QStringLiteral("morph"),
            QStringLiteral("neutral"),
            QStringLiteral("none"),
            QStringLiteral("normal"),
            QStringLiteral("origin"),
            QStringLiteral("passive"),
            QStringLiteral("player"),
            QStringLiteral("portrait"),
            QStringLiteral("psionic"),
            QStringLiteral("radar"),
            QStringLiteral("rawresource"),
            QStringLiteral("ready"),
            QStringLiteral("required"),
            QStringLiteral("robotic"),
            QStringLiteral("self"),
            QStringLiteral("shielded"),
            QStringLiteral("shield"),
            QStringLiteral("shields"),
            QStringLiteral("slow"),
            QStringLiteral("spell"),
            QStringLiteral("stasis"),
            QStringLiteral("stand"),
            QStringLiteral("structure"),
            QStringLiteral("summoned"),
            QStringLiteral("talk"),
            QStringLiteral("target"),
            QStringLiteral("underconstruction"),
            QStringLiteral("uncommandable"),
            QStringLiteral("unstoppable"),
            QStringLiteral("variation"),
            QStringLiteral("visible"),
            QStringLiteral("walk"),
            QStringLiteral("walkfast"),
            QStringLiteral("walkslow"),
            QStringLiteral("work"),
            QStringLiteral("worker")
        };
        for (int i = 1; i <= 16; ++i)
            values.insert(QStringLiteral("user%1").arg(i));
        return values;
    }();

    return tokens.contains(key);
}

inline bool isKnownBlizzardCatalogId(const QString &id)
{
    const QString key = catalogTokenKey(id);
    static const QSet<QString> ids = {
        QStringLiteral("mineralfield"),
        QStringLiteral("mineralfield750"),
        QStringLiteral("richmineralfield"),
        QStringLiteral("richmineralfield750"),
        QStringLiteral("vespenegeyser"),
        QStringLiteral("vespenegeyserhigh"),
        QStringLiteral("spaceplatformgeyser"),
        QStringLiteral("spaceplatformgeyserrich")
    };
    return ids.contains(key);
}

inline bool isDependencyCatalogSource(const QString &sourceFile)
{
    QString normalized = sourceFile;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    normalized = normalized.toLower();
    return normalized.contains(QStringLiteral("/mods/"))
        || normalized.startsWith(QStringLiteral("mods/"))
        || normalized.contains(QStringLiteral(".sc2mod/"))
        || normalized.contains(QStringLiteral("/campaigns/"))
        || normalized.startsWith(QStringLiteral("campaigns/"))
        || normalized.contains(QStringLiteral(".sc2campaign/"));
}

inline bool isEditorRuntimeCatalogType(const QString &elementName)
{
    const QString type = elementName.toLower();
    if (type.isEmpty())
        return false;

    static const QSet<QString> exactTypes = {
        QStringLiteral("ccamera"),
        QStringLiteral("clight"),
        QStringLiteral("cterrain"),
        QStringLiteral("cwater"),
        QStringLiteral("camera"),
        QStringLiteral("light"),
        QStringLiteral("terrain"),
        QStringLiteral("water")
    };
    if (exactTypes.contains(type))
        return true;

    return type.startsWith(QStringLiteral("cterrain"))
        || type.startsWith(QStringLiteral("terrain"))
        || type.startsWith(QStringLiteral("ctexture"))
        || type.startsWith(QStringLiteral("texture"))
        || type.startsWith(QStringLiteral("ccliff"))
        || type.startsWith(QStringLiteral("cliff"));
}

inline bool isEditorRuntimeProtectedNode(const DataNode &node)
{
    return isEditorRuntimeCatalogType(node.elementName);
}

inline bool isProtectedCatalogNode(const DataNode &node)
{
    return isEditorRuntimeProtectedNode(node)
        || isDependencyCatalogSource(node.sourceFile)
        || isKnownBlizzardCatalogId(node.id);
}

inline bool isNonReferenceCatalogFieldName(const QString &name)
{
    const QString field = name.toLower();
    if (field.isEmpty())
        return false;

    return field.contains(QStringLiteral("targetfilters"))
        || field == QStringLiteral("index")
        || field.contains(QStringLiteral("searchfilters"))
        || field.contains(QStringLiteral("autocastfilters"))
        || field.contains(QStringLiteral("filters"))
        || field.contains(QStringLiteral("statuscolors"))
        || field.contains(QStringLiteral("vitalcolors"))
        || field.contains(QStringLiteral("vitalnames"))
        || field.contains(QStringLiteral("vitalmax"))
        || field.contains(QStringLiteral("vitalregen"))
        || field == QStringLiteral("send")
        || field == QStringLiteral("terms")
        || field == QStringLiteral("anim");
}

inline bool looksLikeCatalogFilterList(const QString &value)
{
    if (!value.contains(QLatin1Char(';')))
        return false;

    const QStringList pieces = value.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    int filterTokens = 0;
    for (const QString &piece : pieces) {
        if (isReservedCatalogToken(piece))
            ++filterTokens;
    }
    return filterTokens >= 2;
}

inline bool isSafeAutomaticObjectId(const QString &id)
{
    const QString trimmed = id.trimmed();
    if (trimmed.isEmpty() || trimmed != id)
        return false;
    if (isReservedCatalogToken(trimmed) || isKnownBlizzardCatalogId(trimmed))
        return false;

    bool hasLetter = false;
    for (const QChar ch : trimmed) {
        if (ch.isLetter())
            hasLetter = true;
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('@')))
            return false;
    }
    return hasLetter;
}

} // namespace sc2dh
