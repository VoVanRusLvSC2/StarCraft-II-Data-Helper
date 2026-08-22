#include "core/DecorationStreamingPlanner.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringBuilder>

#include <algorithm>

namespace
{

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
        return value.mid(1, value.size() - 2);
    return value;
}

QString fieldValue(const QString &block, const QString &field)
{
    const QRegularExpression expression(QStringLiteral("\\b%1\\b\\s*(?:=|:)\\s*(\"[^\"]*\"|\\([^)]*\\)|\\{[^}]*\\}|[^\\s,;]+)")
                                            .arg(QRegularExpression::escape(field)),
                                        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(block);
    return match.hasMatch() ? unquote(match.captured(1)) : QString();
}

QVector<double> numbersFrom(const QString &value)
{
    QVector<double> result;
    static const QRegularExpression number(QStringLiteral("[-+]?\\d+(?:\\.\\d+)?"));
    auto matches = number.globalMatch(value);
    while (matches.hasNext())
        result << matches.next().captured(0).toDouble();
    return result;
}

bool containsAny(const QString &value, const QStringList &needles)
{
    for (const QString &needle : needles)
        if (value.contains(needle, Qt::CaseInsensitive))
            return true;
    return false;
}

bool isGalaxyIdentifier(const QString &value)
{
    if (value.isEmpty())
        return false;
    const QChar first = value.front();
    if (!(first.isLetter() || first == QLatin1Char('_')))
        return false;
    for (const QChar ch : value) {
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_')))
            return false;
    }
    return true;
}

QString galaxyString(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"%1\"").arg(value);
}

QString fixed(double value)
{
    QString out = QString::number(value, 'f', 4);
    while (out.contains(QLatin1Char('.')) && out.endsWith(QLatin1Char('0')))
        out.chop(1);
    if (out.endsWith(QLatin1Char('.')))
        out += QLatin1Char('0');
    return out;
}

bool inZone(const sc2dh::decor::DoodadPlacement &doodad, const sc2dh::decor::DecorZone &zone)
{
    const double xMin = std::min(zone.xMin, zone.xMax);
    const double xMax = std::max(zone.xMin, zone.xMax);
    const double yMin = std::min(zone.yMin, zone.yMax);
    const double yMax = std::max(zone.yMin, zone.yMax);
    return doodad.x >= xMin && doodad.x <= xMax && doodad.y >= yMin && doodad.y <= yMax;
}

QString safetyKey(QString value)
{
    return value.trimmed().toCaseFolded();
}

QStringList doodadKeys(const sc2dh::decor::DoodadPlacement &doodad)
{
    QStringList keys;
    const auto append = [&](const QString &value) {
        const QString key = safetyKey(value);
        if (!key.isEmpty())
            keys << key;
    };
    append(doodad.id);
    append(doodad.name);
    return keys;
}

bool isDoodadExcluded(const sc2dh::decor::DoodadPlacement &doodad,
                      const sc2dh::decor::DecorationSafetyContext &context)
{
    for (const QString &key : doodadKeys(doodad)) {
        if (context.excludedDoodadKeys.contains(key))
            return true;
    }
    return false;
}

int forcedZoneIdFor(const sc2dh::decor::DoodadPlacement &doodad,
                    const sc2dh::decor::DecorationSafetyContext &context)
{
    for (const QString &key : doodadKeys(doodad)) {
        const auto it = context.forcedZoneByDoodadKey.constFind(key);
        if (it != context.forcedZoneByDoodadKey.cend())
            return it.value();
    }
    return 0;
}

QString typeStaticOnlyReason(const sc2dh::decor::DoodadPlacement &doodad,
                             const sc2dh::decor::DecorationSafetyContext &context)
{
    const auto it = context.staticOnlyReasonByDoodadType.constFind(safetyKey(doodad.type));
    return it == context.staticOnlyReasonByDoodadType.cend() ? QString() : it.value();
}

QStringList externalReferenceFiles(const sc2dh::decor::DoodadPlacement &doodad,
                                   const sc2dh::decor::DecorationSafetyContext &context)
{
    QStringList files;
    for (const QString &key : doodadKeys(doodad))
        files += context.referenceFilesByDoodadKey.value(key);
    files.removeAll(QString());
    files.removeDuplicates();
    std::sort(files.begin(), files.end(), [](const QString &left, const QString &right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return files;
}

bool injectMapInitCall(QString *script, QString *errorMessage)
{
    if (!script)
        return false;
    static const QRegularExpression initMapExpression(
        QStringLiteral("\\bvoid\\s+InitMap\\s*\\([^)]*\\)\\s*\\{"),
        QRegularExpression::CaseInsensitiveOption);
    if (script->contains(QRegularExpression(QStringLiteral("\\bDecorOpt_Init\\s*\\("))))
        return true;

    const QRegularExpressionMatch match = initMapExpression.match(*script);
    if (!match.hasMatch()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("MapScript.galaxy does not contain a recognizable InitMap() function; decoration runtime was not connected.");
        return false;
    }

    script->insert(match.capturedEnd(), QStringLiteral("\n    DecorOpt_Init();"));
    return true;
}

} // namespace

namespace sc2dh::decor
{

QVector<DoodadPlacement> DecorationStreamingPlanner::parseObjects(const QByteArray &objectsBytes,
                                                                  QStringList *warnings) const
{
    if (warnings)
        warnings->clear();
    const QString text = QString::fromUtf8(objectsBytes);
    QVector<DoodadPlacement> doodads;

    qsizetype searchFrom = 0;
    while (true) {
        const qsizetype start = text.indexOf(QStringLiteral("ObjectDoodad"), searchFrom, Qt::CaseInsensitive);
        if (start < 0)
            break;
        const qsizetype open = text.indexOf(QLatin1Char('{'), start);
        if (open < 0) {
            if (warnings)
                *warnings << QStringLiteral("ObjectDoodad at offset %1 has no opening brace.").arg(start);
            break;
        }

        int depth = 0;
        qsizetype close = -1;
        for (qsizetype i = open; i < text.size(); ++i) {
            if (text.at(i) == QLatin1Char('{'))
                ++depth;
            else if (text.at(i) == QLatin1Char('}')) {
                --depth;
                if (depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        if (close < 0) {
            if (warnings)
                *warnings << QStringLiteral("ObjectDoodad at offset %1 has no closing brace.").arg(start);
            break;
        }

        const QString block = text.mid(open + 1, close - open - 1);
        DoodadPlacement doodad;
        doodad.sourceStart = start;
        doodad.sourceEnd = close + 1;
        doodad.id = fieldValue(block, QStringLiteral("Id"));
        doodad.name = fieldValue(block, QStringLiteral("Name"));
        doodad.type = fieldValue(block, QStringLiteral("Type"));
        doodad.variation = fieldValue(block, QStringLiteral("Variation"));
        doodad.tintColor = fieldValue(block, QStringLiteral("TintColor"));
        doodad.teamColor = fieldValue(block, QStringLiteral("TeamColor"));
        doodad.flags = fieldValue(block, QStringLiteral("Flags"));

        QVector<double> position = numbersFrom(fieldValue(block, QStringLiteral("Position")));
        if (position.size() < 2)
            position = numbersFrom(fieldValue(block, QStringLiteral("Pos")));
        if (position.size() >= 2) {
            doodad.x = position.at(0);
            doodad.y = position.at(1);
            doodad.z = position.size() >= 3 ? position.at(2) : 0.0;
            doodad.hasPosition = true;
        }

        const QVector<double> scale = numbersFrom(fieldValue(block, QStringLiteral("Scale")));
        if (scale.size() == 1) {
            doodad.scaleX = scale.at(0);
            doodad.scaleY = scale.at(0);
            doodad.scaleZ = scale.at(0);
        } else if (scale.size() >= 3) {
            doodad.scaleX = scale.at(0);
            doodad.scaleY = scale.at(1);
            doodad.scaleZ = scale.at(2);
        }

        const QVector<double> rotation = numbersFrom(fieldValue(block, QStringLiteral("Rotation")));
        if (!rotation.isEmpty())
            doodad.rotation = rotation.at(0);
        const QVector<double> pitch = numbersFrom(fieldValue(block, QStringLiteral("Pitch")));
        if (!pitch.isEmpty())
            doodad.pitch = pitch.at(0);
        const QVector<double> roll = numbersFrom(fieldValue(block, QStringLiteral("Roll")));
        if (!roll.isEmpty())
            doodad.roll = roll.at(0);

        if (doodad.type.isEmpty()) {
            doodad.staticOnlyReason = QStringLiteral("Static-only: missing doodad type.");
        } else if (!doodad.hasPosition) {
            doodad.staticOnlyReason = QStringLiteral("Static-only: missing position.");
        } else if (containsAny(doodad.type + QLatin1Char(' ') + doodad.name + QLatin1Char(' ') + doodad.flags,
                              {QStringLiteral("pathing"), QStringLiteral("blocker"),
                               QStringLiteral("footprint"), QStringLiteral("destruct")})) {
            doodad.staticOnlyReason = QStringLiteral("Static-only: pathing/gameplay dependency.");
        } else if (!doodad.variation.isEmpty() || !doodad.tintColor.isEmpty()
                   || !doodad.teamColor.isEmpty() || doodad.pitch != 0.0 || doodad.roll != 0.0) {
            doodad.staticOnlyReason = QStringLiteral("Static-only: unsupported runtime property.");
        } else {
            doodad.dynamicCandidate = true;
        }

        doodads << doodad;
        searchFrom = close + 1;
    }

    return doodads;
}

DecorationStreamingPlan DecorationStreamingPlanner::buildPlan(const QByteArray &objectsBytes,
                                                              const QVector<DecorZone> &zones,
                                                              const DecorationSafetyContext &safetyContext,
                                                              QStringList *warnings) const
{
    DecorationStreamingPlan plan;
    plan.doodads = parseObjects(objectsBytes, &plan.warnings);
    if (warnings)
        *warnings = plan.warnings;

    for (const DecorZone &zone : zones)
    {
        if (zone.id <= 0)
            plan.warnings << QStringLiteral("Decoration zone id must be positive: %1.").arg(zone.id);
        plan.zones << ZoneAssignment{zone.id, {}};
    }
    QSet<int> zoneIds;
    QHash<int, int> zoneIndexById;
    for (int zoneIndex = 0; zoneIndex < zones.size(); ++zoneIndex) {
        const DecorZone &zone = zones.at(zoneIndex);
        if (zoneIds.contains(zone.id))
            plan.warnings << QStringLiteral("Decoration zone id is duplicated: %1.").arg(zone.id);
        zoneIds.insert(zone.id);
        if (zone.id > 0 && !zoneIndexById.contains(zone.id))
            zoneIndexById.insert(zone.id, zoneIndex);
    }

    for (int i = 0; i < plan.doodads.size(); ++i) {
        DoodadPlacement &doodad = plan.doodads[i];
        const QString label = doodad.name.isEmpty() ? (doodad.id.isEmpty() ? doodad.type : doodad.id) : doodad.name;
        doodad.userExcluded = isDoodadExcluded(doodad, safetyContext);
        doodad.forcedZoneId = forcedZoneIdFor(doodad, safetyContext);
        if (doodad.userExcluded && doodad.dynamicCandidate) {
            doodad.dynamicCandidate = false;
            doodad.staticOnlyReason = QStringLiteral("Static-only: excluded by user.");
        }
        if (doodad.dynamicCandidate) {
            const QString typeReason = typeStaticOnlyReason(doodad, safetyContext);
            if (!typeReason.isEmpty()) {
                doodad.dynamicCandidate = false;
                doodad.staticOnlyReason = typeReason;
            }
        }
        if (doodad.dynamicCandidate) {
            doodad.safetyReferenceFiles = externalReferenceFiles(doodad, safetyContext);
            if (!doodad.safetyReferenceFiles.isEmpty()) {
                doodad.dynamicCandidate = false;
                doodad.staticOnlyReason = QStringLiteral("Static-only: pathing/gameplay dependency (referenced by trigger/script text: %1).")
                                              .arg(doodad.safetyReferenceFiles.join(QStringLiteral(", ")));
            }
        }
        if (!doodad.dynamicCandidate) {
            if (doodad.forcedZoneId > 0)
                plan.warnings << QStringLiteral("Doodad %1 has a forced decoration zone %2, but it is static-only: %3")
                                     .arg(label)
                                     .arg(doodad.forcedZoneId)
                                     .arg(doodad.staticOnlyReason);
            plan.staticOnlyDoodads << i;
            continue;
        }

        int assignedZoneIndex = -1;
        if (doodad.forcedZoneId > 0) {
            assignedZoneIndex = zoneIndexById.value(doodad.forcedZoneId, -1);
            if (assignedZoneIndex < 0) {
                plan.warnings << QStringLiteral("Doodad %1 is forced to missing decoration zone %2; leaving it unassigned.")
                                     .arg(label)
                                     .arg(doodad.forcedZoneId);
                plan.unassignedDoodads << i;
                continue;
            }
        } else {
            for (int z = 0; z < zones.size(); ++z) {
                if (!inZone(doodad, zones.at(z)))
                    continue;
                assignedZoneIndex = z;
                break;
            }
        }
        if (assignedZoneIndex < 0) {
            plan.unassignedDoodads << i;
            continue;
        }
        plan.zones[assignedZoneIndex].doodadIndices << i;
    }

    if (warnings)
        *warnings = plan.warnings;
    return plan;
}

DecorationStreamingPlan DecorationStreamingPlanner::buildPlan(const QByteArray &objectsBytes,
                                                              const QVector<DecorZone> &zones,
                                                              QStringList *warnings) const
{
    return buildPlan(objectsBytes, zones, DecorationSafetyContext{}, warnings);
}

QString DecorationStreamingPlanner::generateGalaxy(const DecorationStreamingPlan &plan,
                                                   const GalaxyGenerationOptions &options) const
{
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("NAME_OUT_FUNK") : options.functionPrefix;
    const int batchLimit = std::max(1, options.batchLimit);
    int maxActors = 1;
    for (const ZoneAssignment &zone : plan.zones)
        maxActors = std::max(maxActors, int(zone.doodadIndices.size()));
    const int zoneCount = std::max(1, int(plan.zones.size()));

    QString out;
    out += QStringLiteral("// Generated by SC2 Data Helper Decoration Streaming.\n");
    out += QStringLiteral("// Put this file under scripts/ and include it from the map's script coordinator.\n");
    out += QStringLiteral("const int DecorOpt_ZoneCount = %1;\n").arg(zoneCount);
    out += QStringLiteral("const int DecorOpt_MaxActorsPerZone = %1;\n").arg(maxActors);
    out += QStringLiteral("const int DecorOpt_BatchLimit = %1;\n").arg(batchLimit);
    out += QStringLiteral("actor DecorOpt_Actors[%1][%2];\n").arg(zoneCount + 1).arg(maxActors + 1);
    out += QStringLiteral("int DecorOpt_ActorCount[%1];\n").arg(zoneCount + 1);
    out += QStringLiteral("bool DecorOpt_Loaded[%1];\n\n").arg(zoneCount + 1);

    out += QStringLiteral("void DecorOpt_Init() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_ActorCount[zoneId] = 0;\n");
    out += QStringLiteral("        DecorOpt_Loaded[zoneId] = false;\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_ClearZone(int zoneId) {\n");
    out += QStringLiteral("    int i;\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    out += QStringLiteral("    i = 0;\n");
    out += QStringLiteral("    while (i < DecorOpt_ActorCount[zoneId]) {\n");
    out += QStringLiteral("        ActorSend(DecorOpt_Actors[zoneId][i], \"Destroy\");\n");
    out += QStringLiteral("        i += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("    DecorOpt_ActorCount[zoneId] = 0;\n");
    out += QStringLiteral("    DecorOpt_Loaded[zoneId] = false;\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("void DecorOpt_ClearAll() {\n");
    out += QStringLiteral("    int zoneId;\n");
    out += QStringLiteral("    zoneId = 1;\n");
    out += QStringLiteral("    while (zoneId <= DecorOpt_ZoneCount) {\n");
    out += QStringLiteral("        DecorOpt_ClearZone(zoneId);\n");
    out += QStringLiteral("        zoneId += 1;\n");
    out += QStringLiteral("    }\n");
    out += QStringLiteral("}\n\n");

    out += QStringLiteral("bool DecorOpt_IsZoneLoaded(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return false; }\n");
    out += QStringLiteral("    return DecorOpt_Loaded[zoneId];\n");
    out += QStringLiteral("}\n\n");

    for (const ZoneAssignment &zone : plan.zones) {
        out += QStringLiteral("static void DecorOpt_CreateZone_%1() {\n").arg(zone.zoneId);
        out += QStringLiteral("    point p;\n");
        out += QStringLiteral("    actor a;\n");
        out += QStringLiteral("    int created;\n");
        out += QStringLiteral("    if (DecorOpt_Loaded[%1]) { return; }\n").arg(zone.zoneId);
        out += QStringLiteral("    created = 0;\n");
        for (int doodadIndex : zone.doodadIndices) {
            const DoodadPlacement &doodad = plan.doodads.at(doodadIndex);
            out += QStringLiteral("    p = Point(%1, %2);\n").arg(fixed(doodad.x), fixed(doodad.y));
            out += QStringLiteral("    a = libNtve_gf_CreateActorAtPoint(%1, p);\n").arg(galaxyString(doodad.type));
            out += QStringLiteral("    ActorSend(a, libNtve_gf_SetScale(%1, %2, %3, 0.0));\n")
                       .arg(fixed(doodad.scaleX), fixed(doodad.scaleY), fixed(doodad.scaleZ));
            if (doodad.rotation != 0.0)
                out += QStringLiteral("    ActorSend(a, libNtve_gf_SetFacing(%1));\n").arg(fixed(doodad.rotation));
            if (doodad.z != 0.0)
                out += QStringLiteral("    ActorSend(a, libNtve_gf_SetHeight(%1));\n").arg(fixed(doodad.z));
            out += QStringLiteral("    DecorOpt_Actors[%1][DecorOpt_ActorCount[%1]] = a;\n").arg(zone.zoneId);
            out += QStringLiteral("    DecorOpt_ActorCount[%1] += 1;\n").arg(zone.zoneId);
            out += QStringLiteral("    created += 1;\n");
            out += QStringLiteral("    if (created >= DecorOpt_BatchLimit) { Wait(0.0, c_timeGame); created = 0; }\n");
        }
        out += QStringLiteral("    DecorOpt_Loaded[%1] = true;\n").arg(zone.zoneId);
        out += QStringLiteral("}\n\n");
    }

    out += QStringLiteral("void DecorOpt_CreateZone(int zoneId) {\n");
    out += QStringLiteral("    if (zoneId < 1 || zoneId > DecorOpt_ZoneCount) { return; }\n");
    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("    if (zoneId == %1) { DecorOpt_CreateZone_%1(); return; }\n").arg(zone.zoneId);
    out += QStringLiteral("}\n\n");

    for (const ZoneAssignment &zone : plan.zones)
        out += QStringLiteral("void %1_%2() { DecorOpt_CreateZone(%2); }\n").arg(prefix).arg(zone.zoneId);

    return out;
}

bool DecorationStreamingPlanner::validateGeneratedGalaxy(const QString &galaxySource, QStringList *errors) const
{
    if (errors)
        errors->clear();
    const auto addError = [&](const QString &error) {
        if (errors)
            *errors << error;
    };

    static const QStringList forbidden = {
        QStringLiteral("PPObjects"),
        QStringLiteral("PPLoadMap"),
        QStringLiteral("PPDoodadCreateActor")
    };
    for (const QString &token : forbidden) {
        if (galaxySource.contains(token, Qt::CaseSensitive))
            addError(QStringLiteral("Generated Galaxy uses forbidden helper %1.").arg(token));
    }

    int braces = 0;
    int parens = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype i = 0; i < galaxySource.size(); ++i) {
        const QChar ch = galaxySource.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = true;
            continue;
        }
        if (ch == QLatin1Char('{'))
            ++braces;
        else if (ch == QLatin1Char('}'))
            --braces;
        else if (ch == QLatin1Char('('))
            ++parens;
        else if (ch == QLatin1Char(')'))
            --parens;
        if (braces < 0 || parens < 0)
            addError(QStringLiteral("Generated Galaxy has an unmatched closing bracket near offset %1.").arg(i));
    }
    if (inString)
        addError(QStringLiteral("Generated Galaxy has an unterminated string literal."));
    if (braces != 0)
        addError(QStringLiteral("Generated Galaxy has unbalanced braces."));
    if (parens != 0)
        addError(QStringLiteral("Generated Galaxy has unbalanced parentheses."));

    static const QStringList required = {
        QStringLiteral("void DecorOpt_Init()"),
        QStringLiteral("void DecorOpt_CreateZone(int zoneId)"),
        QStringLiteral("void DecorOpt_ClearZone(int zoneId)"),
        QStringLiteral("void DecorOpt_ClearAll()"),
        QStringLiteral("bool DecorOpt_IsZoneLoaded(int zoneId)")
    };
    for (const QString &signature : required) {
        if (!galaxySource.contains(signature))
            addError(QStringLiteral("Generated Galaxy is missing required API: %1.").arg(signature));
    }

    return !errors || errors->isEmpty();
}

bool DecorationStreamingPlanner::injectGalaxyInclude(const QByteArray &mapScriptBytes,
                                                     const QString &scriptEntry,
                                                     QByteArray *rewrittenMapScriptBytes,
                                                     QString *errorMessage) const
{
    if (!rewrittenMapScriptBytes) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Missing rewritten MapScript output buffer.");
        return false;
    }
    QString normalizedEntry = scriptEntry.trimmed().replace('\\', '/');
    while (normalizedEntry.startsWith(QLatin1Char('/')))
        normalizedEntry.remove(0, 1);
    if (normalizedEntry.isEmpty() || !normalizedEntry.endsWith(QStringLiteral(".galaxy"), Qt::CaseInsensitive)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Generated runtime include path must point to a .galaxy script entry.");
        return false;
    }

    QString script = QString::fromUtf8(mapScriptBytes);
    const QString includeLine = QStringLiteral("include \"%1\"").arg(normalizedEntry);
    const QRegularExpression existingInclude(QStringLiteral("^\\s*include\\s+\"%1\"\\s*;?\\s*$")
                                                 .arg(QRegularExpression::escape(normalizedEntry)),
                                             QRegularExpression::CaseInsensitiveOption
                                                 | QRegularExpression::MultilineOption);

    QString prefix;
    if (script.startsWith(QChar(0xFEFF))) {
        prefix = script.left(1);
        script.remove(0, 1);
    }

    QString rewritten = script;
    if (!existingInclude.match(rewritten).hasMatch()) {
        QString withInclude = includeLine % QStringLiteral("\n");
        if (!rewritten.isEmpty() && !rewritten.startsWith(QLatin1Char('\n')) && !rewritten.startsWith(QLatin1Char('\r')))
            withInclude += QLatin1Char('\n');
        withInclude += rewritten;
        rewritten = withInclude;
    }
    if (!injectMapInitCall(&rewritten, errorMessage))
        return false;
    rewritten = prefix + rewritten;
    *rewrittenMapScriptBytes = rewritten.toUtf8();
    return true;
}

QByteArray DecorationStreamingPlanner::removeDynamicDoodadsFromObjects(const QByteArray &objectsBytes,
                                                                       const DecorationStreamingPlan &plan,
                                                                       QVector<int> *removedDoodadIndices,
                                                                       QStringList *warnings) const
{
    if (removedDoodadIndices)
        removedDoodadIndices->clear();
    if (warnings)
        warnings->clear();

    QSet<int> removeSet;
    for (const ZoneAssignment &zone : plan.zones) {
        for (int doodadIndex : zone.doodadIndices) {
            if (removeSet.contains(doodadIndex)) {
                if (warnings)
                    *warnings << QStringLiteral("Doodad index %1 is assigned to more than one dynamic zone.").arg(doodadIndex);
                continue;
            }
            removeSet.insert(doodadIndex);
        }
    }

    struct Range
    {
        qsizetype start = -1;
        qsizetype end = -1;
        int doodadIndex = -1;
    };
    QVector<Range> ranges;
    const QString text = QString::fromUtf8(objectsBytes);
    for (int doodadIndex : removeSet) {
        if (doodadIndex < 0 || doodadIndex >= plan.doodads.size()) {
            if (warnings)
                *warnings << QStringLiteral("Invalid dynamic doodad index %1.").arg(doodadIndex);
            continue;
        }
        const DoodadPlacement &doodad = plan.doodads.at(doodadIndex);
        if (!doodad.dynamicCandidate) {
            if (warnings)
                *warnings << QStringLiteral("Refused to remove static-only doodad %1 from Objects.")
                                .arg(doodad.name.isEmpty() ? doodad.type : doodad.name);
            continue;
        }
        if (doodad.sourceStart < 0 || doodad.sourceEnd <= doodad.sourceStart || doodad.sourceEnd > text.size()) {
            if (warnings)
                *warnings << QStringLiteral("Refused to remove doodad %1 because its source range is invalid.")
                                .arg(doodad.name.isEmpty() ? doodad.type : doodad.name);
            continue;
        }
        ranges << Range{doodad.sourceStart, doodad.sourceEnd, doodadIndex};
    }

    std::sort(ranges.begin(), ranges.end(), [](const Range &left, const Range &right) {
        return left.start < right.start;
    });
    qsizetype previousEnd = -1;
    QVector<Range> safeRanges;
    for (Range range : ranges) {
        if (range.start < previousEnd) {
            if (warnings)
                *warnings << QStringLiteral("Refused to remove overlapping ObjectDoodad range at offset %1.").arg(range.start);
            continue;
        }
        previousEnd = range.end;
        if (range.end < text.size()) {
            if (text.mid(range.end, 2) == QStringLiteral("\r\n"))
                range.end += 2;
            else if (text.at(range.end) == QLatin1Char('\n') || text.at(range.end) == QLatin1Char('\r'))
                range.end += 1;
        }
        safeRanges << range;
    }

    QString optimized = text;
    std::sort(safeRanges.begin(), safeRanges.end(), [](const Range &left, const Range &right) {
        return left.start > right.start;
    });
    for (const Range &range : safeRanges) {
        optimized.remove(range.start, range.end - range.start);
        if (removedDoodadIndices)
            removedDoodadIndices->prepend(range.doodadIndex);
    }

    return optimized.toUtf8();
}

DecorationOptimizedArtifacts DecorationStreamingPlanner::createOptimizedArtifacts(const QByteArray &objectsBytes,
                                                                                  const QVector<DecorZone> &zones,
                                                                                  const DecorationSafetyContext &safetyContext,
                                                                                  const GalaxyGenerationOptions &options) const
{
    DecorationOptimizedArtifacts artifacts;
    artifacts.plan = buildPlan(objectsBytes, zones, safetyContext, &artifacts.warnings);
    const QString prefix = options.functionPrefix.isEmpty() ? QStringLiteral("NAME_OUT_FUNK") : options.functionPrefix;
    if (!isGalaxyIdentifier(prefix))
        artifacts.warnings << QStringLiteral("Decoration function prefix is not a valid Galaxy identifier: %1.").arg(prefix);
    if (options.batchLimit <= 0)
        artifacts.warnings << QStringLiteral("Decoration batch limit must be positive.");

    QStringList removalWarnings;
    artifacts.optimizedObjectsBytes = removeDynamicDoodadsFromObjects(objectsBytes,
                                                                      artifacts.plan,
                                                                      &artifacts.removedDoodadIndices,
                                                                      &removalWarnings);
    artifacts.warnings += removalWarnings;
    artifacts.galaxySource = generateGalaxy(artifacts.plan, options);
    QStringList galaxyErrors;
    if (!validateGeneratedGalaxy(artifacts.galaxySource, &galaxyErrors))
        artifacts.warnings += galaxyErrors;
    artifacts.valid = artifacts.warnings.isEmpty();
    return artifacts;
}

DecorationOptimizedArtifacts DecorationStreamingPlanner::createOptimizedArtifacts(const QByteArray &objectsBytes,
                                                                                  const QVector<DecorZone> &zones,
                                                                                  const GalaxyGenerationOptions &options) const
{
    return createOptimizedArtifacts(objectsBytes, zones, DecorationSafetyContext{}, options);
}

DecorationArchivePatch DecorationStreamingPlanner::prepareArchivePatch(const QByteArray &objectsBytes,
                                                                       const QByteArray &mapScriptBytes,
                                                                       const QVector<DecorZone> &zones,
                                                                       const DecorationSafetyContext &safetyContext,
                                                                       const GalaxyGenerationOptions &options,
                                                                       const QString &objectsEntry,
                                                                       const QString &mapScriptEntry,
                                                                       const QString &runtimeEntry) const
{
    DecorationArchivePatch patch;
    patch.objectsEntry = objectsEntry.isEmpty() ? QStringLiteral("Objects") : objectsEntry;
    patch.mapScriptEntry = mapScriptEntry.isEmpty() ? QStringLiteral("MapScript.galaxy") : mapScriptEntry;
    patch.runtimeEntry = runtimeEntry.isEmpty() ? QStringLiteral("scripts/sc2dh_decor_opt.galaxy") : runtimeEntry;

    patch.artifacts = createOptimizedArtifacts(objectsBytes, zones, safetyContext, options);
    patch.warnings = patch.artifacts.warnings;
    if (!patch.artifacts.valid) {
        patch.error = QStringLiteral("Decoration optimized artifacts are not valid.");
        return patch;
    }
    if (patch.artifacts.removedDoodadIndices.isEmpty()) {
        patch.error = QStringLiteral("No dynamic visual doodads were assigned to decoration zones.");
        return patch;
    }

    QByteArray rewrittenMapScript;
    if (!injectGalaxyInclude(mapScriptBytes, patch.runtimeEntry, &rewrittenMapScript, &patch.error))
        return patch;

    patch.replacementEntries.insert(patch.objectsEntry, patch.artifacts.optimizedObjectsBytes);
    patch.replacementEntries.insert(patch.runtimeEntry, patch.artifacts.galaxySource.toUtf8());
    patch.replacementEntries.insert(patch.mapScriptEntry, rewrittenMapScript);
    patch.valid = true;
    return patch;
}

DecorationArchivePatch DecorationStreamingPlanner::prepareArchivePatch(const QByteArray &objectsBytes,
                                                                       const QByteArray &mapScriptBytes,
                                                                       const QVector<DecorZone> &zones,
                                                                       const GalaxyGenerationOptions &options,
                                                                       const QString &objectsEntry,
                                                                       const QString &mapScriptEntry,
                                                                       const QString &runtimeEntry) const
{
    return prepareArchivePatch(objectsBytes, mapScriptBytes, zones, DecorationSafetyContext{},
                               options, objectsEntry, mapScriptEntry, runtimeEntry);
}

} // namespace sc2dh::decor
